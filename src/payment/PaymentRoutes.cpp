#include "PaymentRoutes.h"
#include "../core/server/HttpServer.h"
#include "../auth/AuthService.h"
#include "../payment/VPayService.h"
#include "../common/StringUtils.h"
#include "../common/Snowflake.h"
#include "../core/config/Env.h"
#include "../core/database/DbPool.h"
#include "../core/cache/RedisClient.h"
#include <mysql/mysql.h>
#include <sstream>

// 与 PeerRoutes 一致: CF-Connecting-IP > X-Real-IP > X-Forwarded-For
static std::string extractClientIp(const std::string &headers) {
    std::string ip;
    auto findHeader = [&](const std::string &name) -> std::string {
        size_t pos = headers.find("\r\n" + name + ":");
        if (pos == std::string::npos) pos = headers.find(name + ":");
        if (pos == std::string::npos) return "";
        size_t start = headers.find(':', pos) + 1;
        while (start < headers.size() && headers[start] == ' ') start++;
        size_t end = headers.find("\r\n", start);
        if (end == std::string::npos) end = headers.size();
        return headers.substr(start, end - start);
    };
    ip = findHeader("CF-Connecting-IP");
    if (ip.empty()) ip = findHeader("X-Real-IP");
    if (ip.empty()) {
        std::string xff = findHeader("X-Forwarded-For");
        if (!xff.empty()) {
            size_t comma = xff.find(',');
            ip = (comma == std::string::npos) ? xff : xff.substr(0, comma);
            while (!ip.empty() && ip.front() == ' ') ip.erase(ip.begin());
        }
    }
    return ip;
}

// 文件级指针供对账任务使用
static VPayService *g_payService = nullptr;

// ── 对账任务: 每 60s 由 main 循环调用 ──
// 扫描近2小时 pending 订单 → 向平台查单 → 已付即补账。
// 不依赖客户端轮询: 回调丢失 + 客户端关闭也能保证付了钱必发货。
// 每单 5 分钟内只查一次 (Redis 节流), 避免对平台造成查询压力。
void paymentReconcileSweep() {
    if (!g_payService) return;
    // 雪花ID时间下界: now-2h (数据中心/节点/序号取0 = 该时刻最小ID)
    uint64_t cutoff = ((Snowflake::nowMs() - 2ULL * 3600 * 1000 - Snowflake::EPOCH)
                       << Snowflake::TIMESTAMP_SHIFT);
    auto ids = g_payService->pendingOrdersSince(cutoff, 50);
    size_t checked = 0;
    for (auto id : ids) {
        std::string ck = "pay:chk:" + std::to_string(id);
        try {
            if (RedisClient::instance().exists(ck)) continue;
            RedisClient::instance().setex(ck, 300LL, "1");
        } catch (...) {}
        if (g_payService->syncOrderStatus(id)) checked++;
    }
    if (!ids.empty())
        printf("[VPay] sweep: %zu pending scanned, %zu confirmed\n", ids.size(), checked);
}

void registerPaymentRoutes(HttpServer &server, AuthService &auth, DbPool &pool) {

    static VPayService payService(
        pool,
        Env::get("PAY_API_URL", ""),
        Env::get("PAY_APP_ID", ""),
        Env::get("PAY_APP_KEY", ""),
        Env::get("PAY_NOTIFY_URL", ""),
        Env::get("PAY_RETURN_URL", "")
    );
    g_payService = &payService;

    // ── 套餐列表 ──
    server.route("GET", "/api/payment/plans",
        [&](const std::string &, const std::string &, const std::string &) {
            std::string j = "{\"success\":true,\"plans\":["; bool f=true;
            for (auto &p : VPayService::plans()) {
                if (!f) j+=","; f=false;
                j += "{\"product_id\":\""+AuthService::jsonEscape(p.productId)+"\""
                   + ",\"name\":\""+AuthService::jsonEscape(p.name)+"\""
                   + ",\"amount\":"+std::to_string(p.amount)
                   + ",\"days\":"+std::to_string(p.days)+"}";
            }
            j += "]}"; return j;
        });

    // ── 创建订单 (pay_type 可选: alipay 默认 / wxpay / douyinpay) ──
    server.route("POST", "/api/payment/order",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
            uint64_t uid = std::stoull(tp->sub);
            std::string pid = StringUtils::jsonVal(body, "product_id");
            if (pid.empty()) return AuthService::jsonErr("FIELDS_REQUIRED", "请选择套餐");
            std::string payType = StringUtils::jsonVal(body, "pay_type");
            std::string clientIp = extractClientIp(headers);
            auto [ok, result] = payService.createOrder(uid, pid, clientIp, payType);
            if (!ok) return AuthService::jsonErr("ORDER_FAILED", result);
            return "{\"success\":true,"+result.substr(1);
        });

    // ── 回调（GET 请求，RSA 验签）──
    server.route("GET", "/api/payment/callback",
        [&](const std::string &, const std::string &query, const std::string &) {
            bool ok = payService.handleCallback(query);
            return ok ? std::string("success") : std::string("fail");
        });

    // ── 支付完成跳转页 (return_url) ──
    server.route("GET", "/api/payment/return",
        [&](const std::string &, const std::string &, const std::string &) {
            return std::string(
                "<!DOCTYPE html><html lang=\"zh\"><head><meta charset=\"utf-8\">"
                "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                "<title>支付完成</title></head>"
                "<body style=\"margin:0;font-family:sans-serif;background:#0f172a;color:#e2e8f0;"
                "display:flex;align-items:center;justify-content:center;height:100vh\">"
                "<div style=\"text-align:center\">"
                "<div style=\"font-size:52px\">✅</div>"
                "<h1 style=\"color:#10b981;margin:14px 0 8px\">支付成功</h1>"
                "<p style=\"margin:0;color:#94a3b8\">请返回软件，订单将在数秒内到账</p>"
                "</div></body></html>");
        });

    // ── 查询订单状态（含过期检测 + 平台主动查单补账）──────
    server.route("GET", "/api/payment/order/status",
        [&](const std::string &, const std::string &query, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
            std::string oid = StringUtils::queryVal(query, "order_id");
            if (oid.empty()) return AuthService::jsonErr("ID_REQUIRED", "请提供订单ID");
            uint64_t orderId = 0;
            try { orderId = std::stoull(oid); } catch (...) { return AuthService::jsonErr("INVALID", "订单ID无效"); }

            // 先查 Redis — 过期则回退查 DB 真实状态, 不让用户承担后果
            bool expired = true;
            try { auto v = RedisClient::instance().get("pay:order:" + oid); expired = !v; } catch(...) {}

            auto o = payService.queryOrder(orderId);
            if (!o) return AuthService::jsonErr("NOT_FOUND", "订单不存在");

            // 本地未支付 → 平台主动查单补账 (回调丢失兜底)
            if (o->status != "paid") {
                payService.syncOrderStatus(orderId);
                o = payService.queryOrder(orderId);
            }

            if (o->status == "paid")
                return std::string("{\"success\":true,\"status\":\"paid\"}");
            if (expired)
                return std::string("{\"success\":true,\"status\":\"expired\"}");
            std::ostringstream j;
            j<<"{\"success\":true,\"status\":\""<<AuthService::jsonEscape(o->status)<<"\""
             <<",\"id\":"<<o->orderId<<",\"amount\":"<<o->amount
             <<",\"duration_days\":"<<o->durationDays<<"}";
            return j.str();
        });
}
