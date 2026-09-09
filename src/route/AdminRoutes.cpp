#include "AdminRoutes.h"
#include "RouteGuard.h"
// PaymentRoutes.h 提供 paymentService() 全局访问器 (退款路由使用);
// VPayService.h 提供其方法调用的完整类型定义
#include "PaymentRoutes.h"
#include "../payment/VPayService.h"
#include "../core/server/HttpServer.h"
#include "../auth/AuthService.h"
#include "../common/StringUtils.h"
#include "../common/TimeUtils.h"
#include "../core/cache/SubCache.h"
#include "../core/database/DbPool.h"
#include "../welfare/WelfareStore.h"
#include "../redeem/RedeemStore.h"

#include <sstream>
#include <cstdio>
#include <iostream>

void registerAdminRoutes(HttpServer &server, AuthService &auth, DbPool &pool) {
    // 存储层实例 (与 server 同生命周期, lambda 引用安全)
    static WelfareStore welfare(pool);
    static RedeemStore  redeemStore(pool);

    // ── 管理: 修改订阅天数 (相对) ──────────
    server.route("POST", "/api/admin/subscription",
        [&](const std::string &body, const std::string &, const std::string &) {
            std::string email = StringUtils::jsonVal(body, "email");
            int days = std::stoi(StringUtils::jsonVal(body, "days"));
            if (email.empty() || days == 0)
                return AuthService::jsonErr("FIELDS_REQUIRED", "email和days必填");
            int affected = auth.userStore().adjustSubscriptionDays(email, days);
            if (affected < 0)
                return AuthService::jsonErr("DB_ERROR", "db busy");
            if (affected == 0)
                return AuthService::jsonErr("NOT_FOUND", "用户不存在或未订阅");
            // 同步订阅缓存 (唯一实现 SubCache::write, 2026-09-06 收敛)
            {
                auto u = auth.userStore().findByEmail(email);
                if (u) SubCache::write(u->id, u->subscription_expires);
            }
            std::cout << "[Admin] subscription email=" << email << " days=" << days << std::endl;
            return AuthService::jsonOk(days > 0
                ? "已添加 " + std::to_string(days) + " 天"
                : "已减少 " + std::to_string(-days) + " 天");
        }, route::requireAdmin(auth));

    // ── 管理: 查询订阅 ──────────
    server.route("GET", "/api/admin/subscription",
        [&](const std::string &, const std::string &query, const std::string &) {
            std::string email = StringUtils::queryVal(query, "email");
            if (email.empty()) return AuthService::jsonErr("FIELDS_REQUIRED", "email必填");
            auto user = auth.userStore().findByEmail(email);
            if (!user) return AuthService::jsonErr("NOT_FOUND", "用户不存在");
            std::ostringstream resp;
            resp << "{\"success\":true"
                 << ",\"email\":\"" << AuthService::jsonEscape(user->email) << "\""
                 << ",\"username\":\"" << AuthService::jsonEscape(user->username) << "\""
                 << ",\"status\":" << user->status
                 << ",\"subscription_expires\":\"" << AuthService::jsonEscape(user->subscription_expires) << "\"";
            // 计算剩余天数
            if (!user->subscription_expires.empty()) {
                int rem = TimeUtils::subTtlSeconds(user->subscription_expires) / 86400;
                resp << ",\"remaining_days\":" << rem;
            } else { resp << ",\"remaining_days\":0"; }
            resp << "}";
            return resp.str();
        }, route::requireAdmin(auth));

    // ── 管理: 在线用户列表 ──────────
    server.route("GET", "/api/admin/online-users",
        [&](const std::string &, const std::string &, const std::string &) {
            auto users = auth.onlineUsers();
            std::ostringstream resp;
            resp << "{\"success\":true,\"users\":[";
            for (size_t i = 0; i < users.size(); ++i) {
                if (i > 0) resp << ",";
                resp << "{\"uid\":\"" << users[i].uid << "\""
                     << ",\"email\":\"" << StringUtils::jsonEscape(users[i].email) << "\"}";
            }
            resp << "],\"count\":" << users.size() << "}";
            return resp.str();
        }, route::requireAdmin(auth));

    // ── 管理: 踢人下线 ──────────
    server.route("POST", "/api/admin/kick-user",
        [&](const std::string &body, const std::string &, const std::string &) {
            std::string email = StringUtils::jsonVal(body, "email");
            std::string uid   = StringUtils::jsonVal(body, "uid");
            if (email.empty() && uid.empty())
                return AuthService::jsonErr("FIELDS_REQUIRED", "email或uid必填");
            auth.kickUser(uid, email);
            return AuthService::jsonOk("已踢出，用户下次心跳将被强制下线");
        }, route::requireAdmin(auth));

    // ── 管理: 设置订阅到期时间 (绝对值, 覆盖式) ──
    server.route("POST", "/api/admin/set-subscription",
        [&](const std::string &body, const std::string &, const std::string &) {
            std::string email   = StringUtils::jsonVal(body, "email");
            std::string expires = StringUtils::jsonVal(body, "expires");
            if (email.empty() || expires.empty())
                return AuthService::jsonErr("FIELDS_REQUIRED", "email和expires必填");
            auto u = auth.userStore().findByEmail(email);
            if (!u) return AuthService::jsonErr("NOT_FOUND", "用户不存在");
            if (!auth.userStore().setSubscriptionExpires(email, expires))
                return AuthService::jsonErr("DB_ERROR", "db busy");
            // 回写订阅缓存 (唯一实现 SubCache::write; 空串=清除)
            auto nu = auth.userStore().findByEmail(email);
            if (nu) SubCache::write(nu->id, nu->subscription_expires);
            std::cout << "[Admin] set-subscription email=" << email << " expires=" << expires << std::endl;
            return AuthService::jsonOk(expires == "clear"
                ? "已清除订阅到期时间"
                : "已设置到期时间: " + expires);
        }, route::requireAdmin(auth));

    // ── 管理: 定向专属福利 (发到指定用户, 仅该用户可见可领; 领取=加 N 天 Pro) ──
    server.route("POST", "/api/admin/welfare-grant",
        [&](const std::string &body, const std::string &, const std::string &) {
            std::string email = StringUtils::jsonVal(body, "email");
            std::string daysStr = StringUtils::jsonVal(body, "days");
            std::string name = StringUtils::jsonVal(body, "name");
            std::string desc = StringUtils::jsonVal(body, "description");
            std::string expStr = StringUtils::jsonVal(body, "expires_days");
            // 2026-08-26 修复: 空串直接 stoi 抛异常 → 500; 先判空再解析
            if (email.empty() || daysStr.empty() ||
                daysStr.find_first_not_of("0123456789") != std::string::npos)
                return AuthService::jsonErr("FIELDS_REQUIRED", "email和days必填(天数>0)");
            int days = std::stoi(daysStr);
            int expiresDays = expStr.empty() ? 0 : std::stoi(expStr);
            if (name.empty()) name = "专属福利";
            if (desc.empty()) desc = "管理员定向发放";
            if (expiresDays <= 0) expiresDays = 30;
            auto u = auth.userStore().findByEmail(email);
            if (!u) return AuthService::jsonErr("NOT_FOUND", "用户不存在");
            if (!welfare.grantTargeted(email, days, name, desc, expiresDays))
                return AuthService::jsonErr("DB_ERROR", "db busy");
            return AuthService::jsonOk("已向 " + email + " 发放专属福利: " + name
                + " (+" + std::to_string(days) + "天 Pro, " + std::to_string(expiresDays) + " 天内可领)");
        }, route::requireAdmin(auth));

    // ── 管理: 按累计充值金额定向发福利 (2026-08-26) ──
    // 筛选 paid 订单聚合 ≥ min_amount 元的用户, 每人插一条定向 welfare_events
    // 幂等: 同用户+同福利名且未过期 → 跳过 (防 bot 重复执行重复发)
    server.route("POST", "/api/admin/welfare-grant-by-amount",
        [&](const std::string &body, const std::string &, const std::string &) {
            std::string minStr = StringUtils::jsonVal(body, "min_amount");
            std::string daysStr = StringUtils::jsonVal(body, "days");
            std::string name = StringUtils::jsonVal(body, "name");
            std::string desc = StringUtils::jsonVal(body, "description");
            std::string expStr = StringUtils::jsonVal(body, "expires_days");
            // 先判空再解析 (2026-08-26 修复风格: 空串 stoi 抛异常 → 500)
            if (minStr.empty() || daysStr.empty() ||
                minStr.find_first_not_of("0123456789") != std::string::npos ||
                daysStr.find_first_not_of("0123456789") != std::string::npos)
                return AuthService::jsonErr("FIELDS_REQUIRED", "min_amount(元)和days必填(正整数)");
            long long minYuan = std::stoll(minStr);
            int days = std::stoi(daysStr);
            int expiresDays = expStr.empty() ? 0 : std::stoi(expStr);
            if (name.empty()) name = "用户反馈礼包";
            if (desc.empty()) desc = "感谢您的充值支持！请填写反馈问卷，福利已发放";
            if (expiresDays <= 0) expiresDays = 30;
            if (minYuan <= 0 || days <= 0)
                return AuthService::jsonErr("FIELDS_REQUIRED", "min_amount和days须为正数");
            auto gr = welfare.grantByAmount(minYuan, days, name, desc, expiresDays);
            return AuthService::jsonOk("已按累计充值≥" + minStr + "元发放福利 " + name
                + " (+" + std::to_string(days) + "天 Pro, " + std::to_string(expiresDays) + "天内可领): 发放 "
                + std::to_string(gr.granted) + " 人, 跳过 " + std::to_string(gr.skipped) + " 人");
        }, route::requireAdmin(auth));

    // ── 管理: 退款 (仅限有付费记录用户; 全额退, 标记订单 refunded + 取消订阅) ──
    // 边界: 无 paid 订单 → 不改任何数据 (兑换码/福利订阅不受误伤); 幂等; 返回退款总额
    server.route("POST", "/api/admin/refund",
        [&](const std::string &body, const std::string &, const std::string &) {
            std::string email = StringUtils::jsonVal(body, "email");
            if (email.empty())
                return AuthService::jsonErr("FIELDS_REQUIRED", "email必填");
            bool forceRefund = StringUtils::jsonVal(body, "force") == "true";
            auto u = auth.userStore().findByEmail(email);
            if (!u) return AuthService::jsonErr("NOT_FOUND", "用户不存在");
            auto *pay = paymentService();
            if (!pay)
                return AuthService::jsonErr("PAY_SERVICE_UNAVAILABLE", "PAY_SERVICE_UNAVAILABLE");
            auto res = pay->refundUserOrders(u->id, forceRefund);
            if (!res.ok) return AuthService::jsonErr(res.errcode, res.msg);
            if (res.noPaidOrders)
                return AuthService::jsonOk("该用户无付费记录, 未做任何变更");
            char buf[160];
            if (forceRefund)
                snprintf(buf, sizeof(buf), "已标记退款 %lld 笔订单共 %lld 分 (人工已退款), 订阅已取消", res.cnt, res.totalCents);
            else
                snprintf(buf, sizeof(buf), "已退款 %lld 笔订单共 %lld 分, 订阅已取消", res.cnt, res.totalCents);
            std::cout << "[Admin] refund email=" << email << " orders=" << res.cnt
                      << " total_cents=" << res.totalCents << (forceRefund ? " force" : "") << std::endl;
            return AuthService::jsonOk(buf);
        }, route::requireAdmin(auth));

    // ── 管理: 封禁 ──────────
    server.route("POST", "/api/admin/ban",
        [&](const std::string &body, const std::string &, const std::string &) {
            std::string email = StringUtils::jsonVal(body, "email");
            if (email.empty()) return AuthService::jsonErr("FIELDS_REQUIRED", "email必填");
            auto u = auth.userStore().findByEmail(email);
            if (!u) return AuthService::jsonErr("NOT_FOUND", "用户不存在");
            if (!auth.userStore().setStatus(email, 1))
                return AuthService::jsonErr("DB_ERROR", "db busy");
            // 在线立即踢出 (5分钟窗口, 客户端5s心跳必能收到)
            auth.kickUser(std::to_string(u->id), "");
            return AuthService::jsonOk("已封禁 " + email);
        }, route::requireAdmin(auth));

    // ── 管理: 解封 ──────────
    server.route("POST", "/api/admin/unban",
        [&](const std::string &body, const std::string &, const std::string &) {
            std::string email = StringUtils::jsonVal(body, "email");
            if (email.empty()) return AuthService::jsonErr("FIELDS_REQUIRED", "email必填");
            auto u = auth.userStore().findByEmail(email);
            if (!u) return AuthService::jsonErr("NOT_FOUND", "用户不存在");
            if (!auth.userStore().setStatus(email, 0))
                return AuthService::jsonErr("DB_ERROR", "db busy");
            // 清理可能残留的踢出标记 (幂等, 保证解封后立即可登录)
            auth.clearKick(u->id);
            return AuthService::jsonOk("已解封 " + email);
        }, route::requireAdmin(auth));

    // ── 兑换码管理: 生成 (2026-08-16) ──
    server.route("POST", "/api/admin/redeem/generate",
        [&](const std::string &body, const std::string &, const std::string &) {
            auto getNum = [&](const std::string &key, uint64_t def) -> uint64_t {
                std::string v = StringUtils::jsonVal(body, key);
                if (v.empty()) return def;
                try { return std::stoull(v); } catch (...) { return def; }
            };
            uint64_t days    = getNum("days", 0);
            uint64_t count   = getNum("count", 1);
            uint64_t maxUses = getNum("max_uses", 1);    // 0=不限次数
            uint64_t expDays = getNum("expires_days", 0); // 0=永久有效
            std::string prefix = StringUtils::jsonVal(body, "prefix");
            if (prefix.empty()) prefix = "BH";
            auto res = redeemStore.generate(prefix, days, count, maxUses, expDays);
            if (!res.ok) return AuthService::jsonErr(res.errcode, res.msg);

            std::string json = "{\"success\":true,\"message\":\"已生成 " + std::to_string(res.codes.size())
                             + " 个兑换码\",\"codes\":[";
            for (size_t i = 0; i < res.codes.size(); i++) {
                if (i) json += ",";
                json += "\"" + AuthService::jsonEscape(res.codes[i]) + "\"";
            }
            json += "]}";
            return json;
        }, route::requireAdmin(auth));

    // ── 兑换码管理: 列表 (每页50条, created_at 倒序) ──
    server.route("GET", "/api/admin/redeem/list",
        [&](const std::string &, const std::string &query, const std::string &) {
            std::string pageStr = StringUtils::queryVal(query, "page");
            uint64_t page = 1;
            if (!pageStr.empty()) { try { page = std::stoull(pageStr); } catch (...) { page = 1; } }
            auto res = redeemStore.list(page);
            if (!res.ok) return AuthService::jsonErr(res.errcode, res.msg);
            std::string json = "{\"success\":true,\"codes\":[";
            bool first = true;
            for (const auto &r : res.rows) {
                if (!first) json += ","; first = false;
                json += "{\"code\":\"" + AuthService::jsonEscape(r.code)
                     + "\",\"days\":" + std::to_string(r.days)
                     + ",\"max_uses\":" + std::to_string(r.maxUses)
                     + ",\"used_count\":" + std::to_string(r.usedCount)
                     + ",\"is_active\":" + std::to_string(r.isActive)
                     + ",\"expires_at\":\"" + AuthService::jsonEscape(r.expiresAt) + "\""
                     + ",\"created_at\":\"" + AuthService::jsonEscape(r.createdAt) + "\"}";
            }
            json += "],\"total\":" + std::to_string(res.total)
                 +  ",\"active\":" + std::to_string(res.active)
                 +  ",\"total_used\":" + std::to_string(res.totalUsed)
                 +  "}";
            return json;
        }, route::requireAdmin(auth));

    // ── 兑换码管理: 停用 (不影响已兑换用户) ──
    server.route("POST", "/api/admin/redeem/disable",
        [&](const std::string &body, const std::string &, const std::string &) {
            std::string code = StringUtils::jsonVal(body, "code");
            auto res = redeemStore.disable(code);
            if (!res.ok) return AuthService::jsonErr(res.errcode, res.msg);
            return AuthService::jsonOk("已停用兑换码: " + res.code);
        }, route::requireAdmin(auth));
}
