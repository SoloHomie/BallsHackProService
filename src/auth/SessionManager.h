#pragma once
// ── Session / Token 管理（大厂标准双令牌模式）─────────────
//
// Access Token (JWT, 15 min):
//   无状态，签名自验证，不查 Redis。
//   带在 Authorization: Bearer <token> 里。
//
// Refresh Token (随机 hex, 30 天):
//   存在 Redis: "refresh:<token>" → JSON {sub, email, provider}
//   可撤销、可轮换。每次 refresh 旧 token 删除，发新的。
//
// OAuth Token:
//   存在 Redis 缓存（热查询）。
//   Redis key: "oauth:<provider>:<user_id>" TTL = 令牌过期时间。

#include <string>
#include <string_view>
#include <optional>
#include <cstdint>
#include <sstream>
#include <openssl/rand.h>
#include <iostream>
#include "../core/cache/RedisClient.h"

namespace SessionManager {

    // 简易 JSON 取值
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

    // ── Refresh Token 操作 ────────────────────────────

    // 存储 refresh token → 同时加入用户令牌集合
    inline void storeRefresh(const std::string& token,
                             const std::string& userId,
                             const std::string& email,
                             const std::string& provider,
                             int ttlSeconds = 30 * 24 * 3600) {
        try {
            auto& r = RedisClient::instance();
            std::string json = "{\"sub\":\"" + userId + "\""
                             + ",\"email\":\"" + email + "\""
                             + ",\"provider\":\"" + provider + "\"}";
            r.setex("refresh:" + token, ttlSeconds, json);
            // 用户令牌集合，用于批量撤销（改密码等场景）
            r.sadd("user:tokens:" + userId, token);
            r.expire("user:tokens:" + userId, ttlSeconds);
        } catch (const std::exception &e) {
            std::cerr << "[SessionManager] " << __func__ << ": " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[SessionManager] " << __func__ << ": unknown error" << std::endl;
        }
    }

    // 查询 refresh token
    inline std::optional<std::string> getRefresh(const std::string& token) {
        try {
            auto& r = RedisClient::instance();
            auto val = r.get("refresh:" + token);
            if (!val) return std::nullopt;
            return *val;
        } catch (...) { return std::nullopt; }
    }

    // 删除单个 refresh token
    inline void revokeRefresh(const std::string& token) {
        try {
            auto& r = RedisClient::instance();
            auto data = r.get("refresh:" + token);
            if (data) {
                std::string sub = extractJsonVal(*data, "sub");
                if (!sub.empty()) r.srem("user:tokens:" + sub, token);
            }
            r.del("refresh:" + token);
        } catch (const std::exception &e) {
            std::cerr << "[SessionManager] " << __func__ << ": " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[SessionManager] " << __func__ << ": unknown error" << std::endl;
        }
    }

    // 撤销用户所有令牌（修改密码、账号异常时调用）
    inline void revokeAllForUser(uint64_t uid) {
        try {
            auto& r = RedisClient::instance();
            std::string setKey = "user:tokens:" + std::to_string(uid);
            std::vector<std::string> tokens;
            r.smembers(setKey, std::back_inserter(tokens));
            for (auto& t : tokens)
                r.del("refresh:" + t);
            r.del(setKey);
        } catch (const std::exception &e) {
            std::cerr << "[SessionManager] " << __func__ << ": " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[SessionManager] " << __func__ << ": unknown error" << std::endl;
        }
    }

    // ── OAuth Token 缓存（Redis 热层）──────────────────

    inline void cacheOAuthToken(const std::string& provider,
                                uint64_t userId,
                                const std::string& tokenJson,
                                int ttlSeconds) {
        try {
            auto& r = RedisClient::instance();
            std::string key = "oauth:" + provider + ":" + std::to_string(userId);
            r.setex(key, ttlSeconds, tokenJson);
        } catch (const std::exception &e) {
            std::cerr << "[SessionManager] " << __func__ << ": " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[SessionManager] " << __func__ << ": unknown error" << std::endl;
        }
    }

    inline std::optional<std::string> getOAuthCache(
        const std::string& provider, uint64_t userId) {
        try {
            auto& r = RedisClient::instance();
            std::string key = "oauth:" + provider + ":" + std::to_string(userId);
            auto val = r.get(key);
            if (!val) return std::nullopt;
            return *val;
        } catch (...) { return std::nullopt; }
    }

    // ── 令牌即将到期判定 ──────────────────────────────

    // 距离到期 < 5 分钟 就需要刷新
    inline bool needsRefresh(int64_t expiresAt) {
        auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return (expiresAt - now) < 300; // 5 分钟窗口
    }

    // ── JWT ID 生成 (timestamp + crypto-random hex) ───
    inline std::string generateJti() {
        auto now = std::chrono::system_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count();
        unsigned char rnd[8];
        if (RAND_bytes(rnd, sizeof(rnd)) != 1) {
            // fallback: timestamp alone is better than nothing
            return std::to_string(us);
        }
        char hex[17];
        for (int i = 0; i < 8; ++i)
            snprintf(hex + i * 2, 3, "%02x", rnd[i]);
        hex[16] = '\0';
        return std::to_string(us) + "-" + std::string(hex);
    }
}
