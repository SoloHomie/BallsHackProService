#include "WelfareRoutes.h"
#include "../core/server/HttpServer.h"
#include "../auth/AuthService.h"
#include "../common/StringUtils.h"
#include "../common/Snowflake.h"
#include "../core/database/DbPool.h"
#include "../core/cache/RedisClient.h"
#include <mysql/mysql.h>
#include <sstream>
#include <cstdio>
#include <ctime>

static void insertSeedData(MYSQL *db) {
    // 新人福利 — 3天会员（永久有效）
    mysql_query(db,
        "INSERT IGNORE INTO welfare_events (id,name,description,event_type,reward_amount,starts_at,expires_at) "
        "VALUES (1,'新人礼包','欢迎加入球球大家庭！免费领取 3 天 Pro 会员体验~','new_user',3,'2024-01-01','2099-12-31')");
}

void registerWelfareRoutes(HttpServer &server, AuthService &auth, DbPool &pool) {

    // ── 可领取列表 ──
    server.route("GET", "/api/welfare/available",
        [&](const std::string &, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
            uint64_t uid = std::stoull(tp->sub);

            auto guard = pool.acquire(5000);
            if (!guard) return AuthService::jsonErr("DB_ERROR", "数据库繁忙");
            MYSQL *db = guard->get();
            insertSeedData(db);

            std::string now = "NOW()";
            char q[1024];
            snprintf(q, sizeof(q),
                "SELECT e.id, e.name, e.description, e.event_type, e.reward_amount, "
                "e.starts_at, e.expires_at, "
                "CASE WHEN c.id IS NOT NULL THEN 1 ELSE 0 END AS claimed "
                "FROM welfare_events e "
                "LEFT JOIN welfare_claims c ON e.id = c.event_id AND c.user_id = %llu "
                "WHERE e.is_active = 1 AND e.starts_at <= NOW() AND e.expires_at >= NOW() "
                "ORDER BY e.event_type = 'new_user' DESC, e.starts_at ASC",
                uid);
            mysql_query(db, q);
            MYSQL_RES *res = mysql_store_result(db);
            if (!res) return AuthService::jsonErr("DB_ERROR", "数据库繁忙");
            std::string json = "{\"success\":true,\"events\":[";
            bool first = true;
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res))) {
                if (!first) json += ","; first = false;
                json += "{\"id\":" + std::string(row[0])
                     + ",\"name\":\"" + AuthService::jsonEscape(row[1] ? row[1] : "") + "\""
                     + ",\"desc\":\"" + AuthService::jsonEscape(row[2] ? row[2] : "") + "\""
                     + ",\"type\":\"" + AuthService::jsonEscape(row[3] ? row[3] : "") + "\""
                     + ",\"amount\":" + std::string(row[4] ? row[4] : "0")
                     + ",\"starts\":\"" + std::string(row[5] ? row[5] : "") + "\""
                     + ",\"expires\":\"" + std::string(row[6] ? row[6] : "") + "\""
                     + ",\"claimed\":" + std::string(row[7] ? row[7] : "0")
                     + "}";
            }
            json += "]}";
            mysql_free_result(res);
            return json;
        });

    // ── 领取福利 ──
    server.route("POST", "/api/welfare/claim",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
            uint64_t uid = std::stoull(tp->sub);
            std::string eidStr = StringUtils::jsonVal(body, "event_id");
            if (eidStr.empty()) return AuthService::jsonErr("FIELDS_REQUIRED", "请选择福利");
            uint64_t eid = std::stoull(eidStr);

            auto guard = pool.acquire(5000);
            if (!guard) return AuthService::jsonErr("DB_ERROR", "数据库繁忙");
            MYSQL *db = guard->get();

            // 检查活动是否有效
            char q[512];
            snprintf(q, sizeof(q),
                "SELECT id, reward_amount FROM welfare_events "
                "WHERE id=%llu AND is_active=1 AND starts_at<=NOW() AND expires_at>=NOW()", eid);
            mysql_query(db, q);
            MYSQL_RES *res = mysql_store_result(db);
            if (!res || mysql_num_rows(res) == 0) {
                if (res) mysql_free_result(res);
                return AuthService::jsonErr("NOT_AVAILABLE", "该福利已失效");
            }
            MYSQL_ROW row = mysql_fetch_row(res);
            uint64_t amount = std::stoull(row[1] ? row[1] : "0");
            mysql_free_result(res);

            // 检查是否已领取
            snprintf(q, sizeof(q),
                "SELECT id FROM welfare_claims WHERE user_id=%llu AND event_id=%llu", uid, eid);
            mysql_query(db, q);
            res = mysql_store_result(db);
            if (res && mysql_num_rows(res) > 0) {
                mysql_free_result(res);
                return AuthService::jsonErr("ALREADY_CLAIMED", "你已经领取过该福利啦~");
            }
            if (res) mysql_free_result(res);

            uint64_t days = amount;
            uint64_t cid = Snowflake::generate();
            snprintf(q, sizeof(q),
                "INSERT INTO welfare_claims (id, user_id, event_id) VALUES (%llu,%llu,%llu)",
                cid, uid, eid);
            if (mysql_query(db, q) != 0)
                return AuthService::jsonErr("CLAIM_FAILED", "领取失败，请稍后重试");

            // 发放 Pro 订阅天数
            snprintf(q, sizeof(q),
                "UPDATE users SET subscription_expires = "
                "DATE_ADD(GREATEST(COALESCE(subscription_expires, NOW()), NOW()), INTERVAL %llu DAY) "
                "WHERE id=%llu", days, uid);
            mysql_query(db, q);

            // 读出新过期时间，写回 Redis（用 SETEX 而非 DEL，避免竞态）
            std::string newExp;
            {
                snprintf(q, sizeof(q),
                    "SELECT subscription_expires FROM users WHERE id=%llu", uid);
                mysql_query(db, q);
                MYSQL_RES *res2 = mysql_store_result(db);
                if (res2) {
                    MYSQL_ROW row2 = mysql_fetch_row(res2);
                    if (row2 && row2[0]) newExp = row2[0];
                    mysql_free_result(res2);
                }
                if (!newExp.empty()) {
                    // 计算 TTL
                    struct tm et = {};
                    std::istringstream iss(newExp);
                    iss >> std::get_time(&et, "%Y-%m-%d %H:%M:%S");
                    auto tt = std::mktime(&et);
                    int ttl = (int)(tt - std::time(nullptr));
                    if (ttl < 60) ttl = 60;
                    try {
                        auto &r = RedisClient::instance();
                        r.setex("sub:" + tp->sub, ttl, newExp);
                    } catch(...) {}
                }
            }

            // 拼装响应（含最新订阅状态，客户端可立即更新）
            {
                std::ostringstream resp;
                resp << "{\"success\":true"
                     << ",\"message\":\"领取成功！获得 " << days << " 天 Pro 会员~\""
                     << ",\"days\":" << days
                     << ",\"subscribed\":true";
                if (!newExp.empty())
                    resp << ",\"subscription_expires\":\"" << AuthService::jsonEscape(newExp) << "\"";
                else
                    resp << ",\"subscription_expires\":\"\"";
                resp << "}";
                return resp.str();
            }
        });
}
