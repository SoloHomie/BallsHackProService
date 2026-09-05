#pragma once
// ── 订阅缓存回写 (唯一实现, 2026-09-06 收敛) ─────────────
// 各业务 (兑换/福利/管理接口) 修改用户订阅到期时间后, 必须调用
// SubCache::write 同步 Redis sub:<uid> 缓存, 不要再手写 setex。
// 规则: 空串=清除缓存; TTL=距过期秒数(至少60s); 异常仅记录不抛出。
#include "RedisClient.h"
#include "../../common/TimeUtils.h"
#include <iostream>
#include <string>

namespace SubCache {

inline void write(uint64_t uid, const std::string &expires) {
    try {
        auto &r = RedisClient::instance();
        if (expires.empty()) {
            r.del("sub:" + std::to_string(uid));
            return;
        }
        int ttl = TimeUtils::subTtlSeconds(expires);
        if (ttl < 60) ttl = 60;
        r.setex("sub:" + std::to_string(uid), ttl, expires);
    } catch (const std::exception &e) {
        std::cerr << "[SubCache] write FAILED sub:" << uid << ": " << e.what() << std::endl;
    }
}

} // namespace SubCache
