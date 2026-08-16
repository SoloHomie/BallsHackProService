#pragma once
// ── Session / Token 管理（大厂标准双令牌模式）─────────────────
//
// Access Token (JWT, 15 min):
//   无状态，签名自验证，不查任何存储。
//   Authorization: Bearer <token>
//
// Refresh Token (随机 hex, 30 天):
//   Redis 主存储 (自动登录/刷新 1-2ms, 云盘卡顿免疫), MySQL 写备份兜底。
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
#include <ctime>
#include <iomanip>
#include <iterator>
#include <unordered_set>
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
    //  Refresh Token 操作（Redis 主存储 + MySQL 写备份）
    //  读: 先 Redis (1-2ms, 云盘卡顿免疫), miss 回源 MySQL 并回写。
    //  key: refresh:<sha256(token)> → {"sub","email","provider"}
    //       refresh:user:<uid> → SET of token hashes (批量撤销/单设备登录)
    // ═══════════════════════════════════════════════════════

    inline std::string refreshKey(const std::string& hex) { return "refresh:" + hex; }
    inline std::string refreshUserKey(const std::string& uid) { return "refresh:user:" + uid; }

    inline std::string refreshJson(const std::string& uid,
                                   const std::string& email,
                                   const std::string& provider) {
        return "{\"sub\":\"" + uid + "\""
             + ",\"email\":\"" + email + "\""
             + ",\"provider\":\"" + provider + "\"}";
    }

    // 存储 refresh token → Redis (主) + MySQL (备份, 失败不影响登录)
    inline void storeRefresh(const std::string& token,
                             const std::string& userId,
                             const std::string& email,
                             const std::string& provider,
                             int ttlSeconds = 30 * 24 * 3600,
                             DbPool *pool = nullptr) {
        std::string hex = sha256hex(token);
        try {
            auto& r = RedisClient::instance();
            r.setex(refreshKey(hex), ttlSeconds, refreshJson(userId, email, provider));
            r.sadd(refreshUserKey(userId), hex);
            r.expire(refreshUserKey(userId), ttlSeconds);
        } catch (...) {}

        if (!pool) return;
        auto guard = pool->acquire(5000);
        if (!guard) return;
        MYSQL *db = guard->get();

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

    // 查询 refresh token → 先 Redis, miss 回源 MySQL 并回写
    inline std::optional<std::string> getRefresh(const std::string& token,
                                                  DbPool *pool = nullptr) {
        std::string hex = sha256hex(token);
        try {
            auto& r = RedisClient::instance();
            auto val = r.get(refreshKey(hex));
            if (val) return *val;
        } catch (...) {}

        if (!pool) return std::nullopt;
        auto guard = pool->acquire(5000);
        if (!guard) return std::nullopt;
        MYSQL *db = guard->get();

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
        std::string expiresAt = row[3] ? row[3] : "";
        mysql_free_result(res);

        if (uid.empty()) return std::nullopt;

        // 回写 Redis, TTL = 剩余有效期
        long long remain = 3600;
        std::tm et = {};
        std::istringstream es(expiresAt);
        es >> std::get_time(&et, "%Y-%m-%d %H:%M:%S");
        if (!es.fail()) {
            auto etT = std::chrono::system_clock::from_time_t(std::mktime(&et));
            auto nowT = std::chrono::system_clock::now();
            remain = (long long)std::chrono::duration_cast<std::chrono::seconds>(etT - nowT).count();
        }
        if (remain <= 0) return std::nullopt;
        try {
            auto& r = RedisClient::instance();
            r.setex(refreshKey(hex), (int)remain, refreshJson(uid, email, provider));
            r.sadd(refreshUserKey(uid), hex);
            r.expire(refreshUserKey(uid), (int)remain);
        } catch (...) {}

        return refreshJson(uid, email, provider);
    }

    // 删除单个 refresh token (Redis 主 + MySQL 备份)
    inline void revokeRefresh(const std::string& token, DbPool *pool = nullptr) {
        std::string hex = sha256hex(token);
        try {
            auto& r = RedisClient::instance();
            auto val = r.get(refreshKey(hex));
            if (val) {
                std::string sub = extractJsonVal(*val, "sub");
                if (!sub.empty()) r.srem(refreshUserKey(sub), hex);
            }
            r.del(refreshKey(hex));
        } catch (...) {}

        if (!pool) return;
        auto guard = pool->acquire(5000);
        if (!guard) return;
        MYSQL *db = guard->get();

        char q[256];
        snprintf(q, sizeof(q),
            "DELETE FROM refresh_tokens WHERE token_hash='%s'", hex.c_str());
        mysql_query(db, q);
    }

    // 撤销用户所有令牌（修改密码、账号异常时调用）— Redis 主 + MySQL 备份
    inline void revokeAllForUser(uint64_t uid, DbPool *pool = nullptr) {
        std::string uidStr = std::to_string(uid);
        try {
            auto& r = RedisClient::instance();
            std::unordered_set<std::string> hashes;
            r.smembers(refreshUserKey(uidStr), std::inserter(hashes, hashes.begin()));
            for (auto& h : hashes) r.del(refreshKey(h));
            r.del(refreshUserKey(uidStr));
        } catch (...) {}

        if (!pool) return;
        auto guard = pool->acquire(5000);
        if (!guard) return;
        MYSQL *db = guard->get();

        char q[256];
        snprintf(q, sizeof(q),
            "DELETE FROM refresh_tokens WHERE user_id=%llu", (unsigned long long)uid);
        mysql_query(db, q);
    }

    // 定期清理过期 token（MySQL 侧; Redis 靠 TTL 自动过期）
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
