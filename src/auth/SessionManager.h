#pragma once
// ── Session / Token 管理（大厂标准双令牌模式）─────────────────
//
// Access Token (JWT, 15 min):
//   无状态，签名自验证，不查任何存储。
//   Authorization: Bearer <token>
//
// Refresh Token (随机 hex, 30 天):
//   MySQL 是唯一数据源（带索引）。
//   支持轮换：每次刷新时旧 token 失效，发新 token。
//   撤销：按 token 单条撤销 / 按用户批量撤销。
//
// OAuth Token:
//   Redis 缓存（热查询，TTL = 令牌过期时间）。

#include <string>
#include <string_view>
#include <optional>
#include <cstdint>
#include <sstream>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <iostream>
#include "../core/cache/RedisClient.h"
#include "../core/database/DbPool.h"

namespace SessionManager {

    // ── 简易 JSON 取值 ──────────────────────────────────
    inline std::string extractJsonVal(const std::string& json,
                                       const std::string& key) {
        std::string pat = "\"" + key + "\":\"";
        auto pos = json.find(pat);
        if (pos == std::string::npos) return {};
        pos += pat.size();
        auto end = json.find('"', pos);
        if (end == std::string::npos) return {};
        return json.substr(pos, end - pos);
    }

    // ── SHA256 hash (hex) ────────────────────────────────
    inline std::string sha256hex(const std::string& data) {
        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256((const unsigned char*)data.c_str(), data.size(), hash);
        char hex[65];
        for (int i = 0; i < 32; i++) snprintf(hex + i*2, 3, "%02x", hash[i]);
        hex[64] = 0;
        return std::string(hex);
    }

    // ═══════════════════════════════════════════════════════
    //  Refresh Token 操作（MySQL 主存储，无 Redis 缓存）
    // ═══════════════════════════════════════════════════════

    // 存储 refresh token → MySQL
    inline void storeRefresh(const std::string& token,
                             const std::string& userId,
                             const std::string& email,
                             const std::string& provider,
                             int ttlSeconds = 30 * 24 * 3600,
                             DbPool *pool = nullptr) {
        if (!pool) return;
        auto guard = pool->acquire(5000);
        if (!guard) return;
        MYSQL *db = guard->get();

        std::string hex = sha256hex(token);

        // Snowflake ID
        auto now = std::chrono::system_clock::now();
        auto id = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count();

        char expireStr[32];
        time_t expireT = time(nullptr) + ttlSeconds;
        strftime(expireStr, sizeof(expireStr), "%Y-%m-%d %H:%M:%S", gmtime(&expireT));

        // 转义字符串防注入
        char eEmail[512], eProv[128];
        mysql_real_escape_string(db, eEmail, email.c_str(), email.size());
        mysql_real_escape_string(db, eProv, provider.c_str(), provider.size());

        char q[1024];
        snprintf(q, sizeof(q),
            "INSERT INTO refresh_tokens (id,token_hash,user_id,email,provider,expires_at) "
            "VALUES(%lld,'%s',%s,'%s','%s','%s') "
            "ON DUPLICATE KEY UPDATE expires_at='%s'",
            (long long)id, hex.c_str(), userId.c_str(),
            eEmail, eProv, expireStr, expireStr);
        mysql_query(db, q);
    }

    // 查询 refresh token → 直接查 MySQL
    inline std::optional<std::string> getRefresh(const std::string& token,
                                                  DbPool *pool = nullptr) {
        if (!pool) return std::nullopt;
        auto guard = pool->acquire(5000);
        if (!guard) return std::nullopt;
        MYSQL *db = guard->get();

        std::string hex = sha256hex(token);
        char q[512];
        snprintf(q, sizeof(q),
            "SELECT user_id, email, provider, expires_at "
            "FROM refresh_tokens WHERE token_hash='%s' AND expires_at > NOW()",
            hex.c_str());

        if (mysql_query(db, q) != 0) return std::nullopt;

        MYSQL_RES *res = mysql_store_result(db);
        if (!res || mysql_num_rows(res) == 0) {
            if (res) mysql_free_result(res);
            return std::nullopt;
        }

        MYSQL_ROW row = mysql_fetch_row(res);
        std::string uid = row[0] ? row[0] : "";
        std::string email = row[1] ? row[1] : "";
        std::string provider = row[2] ? row[2] : "";
        mysql_free_result(res);

        if (uid.empty()) return std::nullopt;

        return "{\"sub\":\"" + uid + "\""
             + ",\"email\":\"" + email + "\""
             + ",\"provider\":\"" + provider + "\"}";
    }

    // 删除单个 refresh token
    inline void revokeRefresh(const std::string& token, DbPool *pool = nullptr) {
        if (!pool) return;
        auto guard = pool->acquire(5000);
        if (!guard) return;
        MYSQL *db = guard->get();

        std::string hex = sha256hex(token);
        char q[256];
        snprintf(q, sizeof(q),
            "DELETE FROM refresh_tokens WHERE token_hash='%s'", hex.c_str());
        mysql_query(db, q);
    }

    // 撤销用户所有令牌（修改密码、账号异常时调用）
    inline void revokeAllForUser(uint64_t uid, DbPool *pool = nullptr) {
        if (!pool) return;
        auto guard = pool->acquire(5000);
        if (!guard) return;
        MYSQL *db = guard->get();

        char q[256];
        snprintf(q, sizeof(q),
            "DELETE FROM refresh_tokens WHERE user_id=%llu", (unsigned long long)uid);
        mysql_query(db, q);
    }

    // 定期清理过期 token（可定时任务调用）
    inline void purgeExpired(DbPool *pool = nullptr) {
        if (!pool) return;
        auto guard = pool->acquire(5000);
        if (!guard) return;
        MYSQL *db = guard->get();
        mysql_query(db, "DELETE FROM refresh_tokens WHERE expires_at < NOW()");
    }

    // ═══════════════════════════════════════════════════════
    //  OAuth Token 缓存（Redis 热层）
    // ═══════════════════════════════════════════════════════

    inline void cacheOAuthToken(const std::string& provider,
                                uint64_t userId,
                                const std::string& tokenJson,
                                int ttlSeconds) {
        try {
            auto& r = RedisClient::instance();
            std::string key = "oauth:" + provider + ":" + std::to_string(userId);
            r.setex(key, ttlSeconds, tokenJson);
        } catch (...) {}
    }

    inline std::optional<std::string> getOAuthCache(
        const std::string& provider, uint64_t userId) {
        try {
            auto& r = RedisClient::instance();
            auto val = r.get("oauth:" + provider + ":" + std::to_string(userId));
            if (!val) return std::nullopt;
            return *val;
        } catch (...) { return std::nullopt; }
    }

    // ═══════════════════════════════════════════════════════
    //  工具
    // ═══════════════════════════════════════════════════════

    // 距离到期 < 5 分钟就需要刷新
    inline bool needsRefresh(int64_t expiresAt) {
        auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return (expiresAt - now) < 300;
    }

    // JWT ID 生成 (timestamp + crypto-random hex)
    inline std::string generateJti() {
        auto now = std::chrono::system_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count();
        unsigned char rnd[8];
        if (RAND_bytes(rnd, sizeof(rnd)) != 1) {
            return std::to_string(us);
        }
        char hex[17];
        for (int i = 0; i < 8; ++i)
            snprintf(hex + i * 2, 3, "%02x", rnd[i]);
        hex[16] = '\0';
        return std::to_string(us) + "-" + std::string(hex);
    }
}
