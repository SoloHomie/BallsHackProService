#include "RedeemRoutes.h"
#include "../core/server/HttpServer.h"
#include "../auth/AuthService.h"
#include "../common/StringUtils.h"
#include "../common/Snowflake.h"
#include "../core/database/DbPool.h"
#include "../core/cache/RedisClient.h"
#include <mysql/mysql.h>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <sstream>

// 兑换码兑换 Pro 时长 (2026-08-16)
// 一致性设计:
//   - MySQL 事务 (START TRANSACTION/COMMIT/ROLLBACK) 保证"扣次数+加时长"原子
//   - redeem_records 唯一键 uk_user_code 是防重复兑换的总闸 (连点双发也挡住)
//   - 条件 UPDATE (used_count<max_uses) + affected_rows==1 原子抢占最后一份
//   - Redis 限流 10次/10分钟防暴力猜码
void registerRedeemRoutes(HttpServer &server, AuthService &auth, DbPool &pool) {

    server.route("POST", "/api/welfare/redeem",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
            uint64_t uid = std::stoull(tp->sub);

            // ── 码规范化: 全角转半角 + trim + 大写 ──
            std::string raw = StringUtils::jsonVal(body, "code");
            std::string norm;
            for (size_t i = 0; i < raw.size(); ) {
                unsigned char b = raw[i];
                if (b == 0xE3 && i + 2 < raw.size() && (unsigned char)raw[i+1] == 0x80 && (unsigned char)raw[i+2] == 0x80) {
                    norm += ' '; i += 3; continue;                 // U+3000 全角空格
                }
                if (b == 0xEF && i + 2 < raw.size() && (unsigned char)raw[i+1] == 0xBC) {
                    unsigned char c3 = raw[i+2];
                    if (c3 >= 0x90 && c3 <= 0x99) { norm += (char)('0' + (c3 - 0x90)); i += 3; continue; } // 全角数字
                    if (c3 >= 0xA1 && c3 <= 0xBA) { norm += (char)('A' + (c3 - 0xA1)); i += 3; continue; } // 全角大写
                    if (c3 == 0x8D) { norm += '-'; i += 3; continue; }                                     // 全角连字符
                }
                norm += (char)b; i += 1;
            }
            std::string code;
            size_t s = 0, e = norm.size();
            while (s < e && std::isspace((unsigned char)norm[s])) s++;
            while (e > s && std::isspace((unsigned char)norm[e-1])) e--;
            for (size_t k = s; k < e; k++) code += (char)std::toupper((unsigned char)norm[k]);
            if (code.empty() || code.size() > 24)
                return AuthService::jsonErr("INVALID_CODE", "请输入有效的兑换码");
            for (char c : code)
                if (!(std::isalnum((unsigned char)c) || c == '-'))
                    return AuthService::jsonErr("INVALID_CODE", "兑换码格式不正确");

            // ── Redis 限流: 10次/10分钟 ──
            try {
                auto &r = RedisClient::instance();
                auto cnt = r.get("rl:redeem:" + std::to_string(uid));
                int c = cnt ? std::stoi(*cnt) : 0;
                if (c >= 10)
                    return AuthService::jsonErr("TOO_MANY_ATTEMPTS", "尝试次数过多，请10分钟后再试");
                r.incr("rl:redeem:" + std::to_string(uid));
                r.expire("rl:redeem:" + std::to_string(uid), 600);
            } catch (...) {}

            auto guard = pool.acquire(5000);
            if (!guard) return AuthService::jsonErr("DB_ERROR", "数据库繁忙");
            MYSQL *db = guard->get();
            char q[1024]; char eesc[64];
            mysql_real_escape_string(db, eesc, code.c_str(), code.size());

            auto rollbackAnd = [&](const std::string &errcode, const std::string &msg) -> std::string {
                mysql_query(db, "ROLLBACK");
                return AuthService::jsonErr(errcode, msg);
            };

            if (mysql_query(db, "START TRANSACTION") != 0)
                return AuthService::jsonErr("DB_ERROR", "数据库繁忙，请稍后再试");

            // 1. 查码 (FOR UPDATE 串行化同一码的并发兑换)
            snprintf(q, sizeof(q),
                "SELECT id, days, is_active, max_uses, used_count, expires_at, NOW() "
                "FROM redeem_codes WHERE code='%s' FOR UPDATE", eesc);
            mysql_query(db, q);
            MYSQL_RES *res = mysql_store_result(db);
            if (!res || mysql_num_rows(res) == 0) {
                if (res) mysql_free_result(res);
                return rollbackAnd("INVALID_CODE", "兑换码不存在或已失效");
            }
            MYSQL_ROW row = mysql_fetch_row(res);
            uint64_t codeId  = std::stoull(row[0] ? row[0] : "0");
            uint64_t days    = std::stoull(row[1] ? row[1] : "0");
            bool active      = row[2] && std::stoi(row[2]) != 0;
            uint64_t maxUses = std::stoull(row[3] ? row[3] : "0");
            uint64_t used    = std::stoull(row[4] ? row[4] : "0");
            std::string expAt  = row[5] ? row[5] : "";
            std::string nowStr = row[6] ? row[6] : "";
            mysql_free_result(res);

            // 2a. 同一用户重复兑换检查 (先于耗尽预检, 给出准确提示)
            snprintf(q, sizeof(q),
                "SELECT id FROM redeem_records WHERE user_id=%llu AND code_id=%llu", uid, codeId);
            mysql_query(db, q);
            MYSQL_RES *resDup = mysql_store_result(db);
            bool alreadyRedeemed = resDup && mysql_num_rows(resDup) > 0;
            if (resDup) mysql_free_result(resDup);
            if (alreadyRedeemed)
                return rollbackAnd("ALREADY_REDEEMED", "你已兑换过该兑换码~");

            // 2b. 状态预检
            if (!active)  return rollbackAnd("INVALID_CODE", "兑换码已被停用");
            if (days == 0) return rollbackAnd("INVALID_CODE", "兑换码无有效时长");
            if (!expAt.empty() && !nowStr.empty() && expAt < nowStr)
                return rollbackAnd("CODE_EXPIRED", "兑换码已过期");
            if (maxUses != 0 && used >= maxUses)
                return rollbackAnd("CODE_EXHAUSTED", "兑换码已用完");

            // 3. 落兑换流水 —— 唯一键 uk_user_code 是防重复兑换总闸
            uint64_t rid = Snowflake::generate();
            snprintf(q, sizeof(q),
                "INSERT INTO redeem_records (id, user_id, code_id, code, days) "
                "VALUES (%llu,%llu,%llu,'%s',%llu)", rid, uid, codeId, eesc, days);
            if (mysql_query(db, q) != 0) {
                if (mysql_errno(db) == 1062)
                    return rollbackAnd("ALREADY_REDEEMED", "你已兑换过该兑换码~");
                return rollbackAnd("REDEEM_FAILED", "兑换失败，请稍后再试");
            }

            // 4. 条件消费次数 —— affected_rows==1 才原子抢到最后一份
            snprintf(q, sizeof(q),
                "UPDATE redeem_codes SET used_count=used_count+1 "
                "WHERE id=%llu AND is_active=1 "
                "AND (max_uses=0 OR used_count<max_uses) "
                "AND (expires_at IS NULL OR expires_at>=NOW())", codeId);
            if (mysql_query(db, q) != 0 || mysql_affected_rows(db) != 1)
                return rollbackAnd("CODE_EXHAUSTED", "兑换码已用完");

            // 5. 累加订阅时长 (过期从今起算, 未过期累加)
            snprintf(q, sizeof(q),
                "UPDATE users SET subscription_expires = "
                "DATE_ADD(GREATEST(COALESCE(subscription_expires, NOW()), NOW()), INTERVAL %llu DAY) "
                "WHERE id=%llu", days, uid);
            if (mysql_query(db, q) != 0)
                return rollbackAnd("REDEEM_FAILED", "兑换失败，请稍后再试");

            if (mysql_query(db, "COMMIT") != 0) {
                mysql_query(db, "ROLLBACK");
                return AuthService::jsonErr("REDEEM_FAILED", "兑换失败，请稍后再试");
            }

            // 6. 回读新到期时间 → Redis 缓存
            std::string newExp;
            snprintf(q, sizeof(q), "SELECT subscription_expires FROM users WHERE id=%llu", uid);
            mysql_query(db, q);
            MYSQL_RES *res2 = mysql_store_result(db);
            if (res2) {
                MYSQL_ROW r2 = mysql_fetch_row(res2);
                if (r2 && r2[0]) newExp = r2[0];
                mysql_free_result(res2);
            }
            if (!newExp.empty()) {
                struct tm et = {};
                std::istringstream iss(newExp);
                iss >> std::get_time(&et, "%Y-%m-%d %H:%M:%S");
                int ttl = (int)(std::mktime(&et) - std::time(nullptr));
                if (ttl < 60) ttl = 60;
                try {
                    auto &r = RedisClient::instance();
                    r.setex("sub:" + tp->sub, ttl, newExp);
                } catch (...) {}
            }

            std::string msg = "兑换成功！获得 " + std::to_string(days) + " 天 Pro 会员~";
            return "{\"success\":true,\"message\":\"" + AuthService::jsonEscape(msg)
                 + "\",\"days\":" + std::to_string(days)
                 + ",\"subscribed\":true"
                 + ",\"subscription_expires\":\"" + AuthService::jsonEscape(newExp) + "\"}";
        });
}
