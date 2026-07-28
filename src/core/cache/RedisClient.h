#pragma once
// ── Redis connection wrapper ──────────────────────────
// Thin singleton. Call instance() to get a Redis connection.
// Set SUREITY_REDIS_URL env var to configure.
//
// When compiled with -DSUREITY_NO_REDIS, all operations become no-ops
// and the server runs in degraded mode (no caching, no session tokens).

#include "../config/Env.h"
#include <string>

#ifdef SUREITY_NO_REDIS
// ── No-op stub ───────────────────────────────────────
#include <optional>
#include <vector>
#include <cstdint>

class RedisClient {
public:
    struct NoopRedis {
        // get / setex
        std::optional<std::string> get(const std::string&) { return std::nullopt; }
        void setex(const std::string&, int, const std::string&) {}
        void del(const std::string&) {}

        // key checks
        bool exists(const std::string&) { return false; }
        long long ttl(const std::string&) { return -2; }

        // set operations
        void sadd(const std::string&, const std::string&) {}
        void srem(const std::string&, const std::string&) {}
        template<typename T>
        void smembers(const std::string&, T) {}
        void expire(const std::string&, int) {}
    };

    static NoopRedis& instance() {
        static NoopRedis s;
        return s;
    }
};
#else
// ── Real Redis (redis-plus-plus) ─────────────────────
#include <sw/redis++/redis++.h>
#include <memory>

class RedisClient {
public:
    static sw::redis::Redis &instance() {
        static RedisClient s;
        return *s.m_redis;
    }

private:
    RedisClient() {
        std::string url = Env::get("BH_REDIS_URL", "tcp://127.0.0.1:6380");
        m_redis = std::make_unique<sw::redis::Redis>(url);
    }
    std::unique_ptr<sw::redis::Redis> m_redis;
};
#endif
