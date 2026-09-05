#pragma once
// ── 订阅到期时间工具 (header-only) ─────────────────────
// 订阅判定唯一实现 (2026-09-06 收敛): subscription_expires 为
// "%Y-%m-%d %H:%M:%S" 字符串, 大于当前时间 = 有效。
// 原 AuthService/AuthRoutes/SystemRoutes/AssetRoutes 各有一份手写解析,
// 已全部收敛到这里, 新增订阅逻辑必须复用本文件, 不要再手写 get_time 解析。
#include <string>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <chrono>

namespace TimeUtils {

/// 解析 "%Y-%m-%d %H:%M:%S" → time_t; 格式非法返回 false
inline bool parseExpires(const std::string &expires, std::time_t &outT) {
    if (expires.empty()) return false;
    struct tm t = {};
    std::istringstream ss(expires);
    ss >> std::get_time(&t, "%Y-%m-%d %H:%M:%S");
    if (ss.fail()) return false;
    outT = std::mktime(&t);
    return true;
}

/// 订阅是否有效 (空串/格式错 = 无效)
inline bool subActive(const std::string &expires) {
    std::time_t t;
    if (!parseExpires(expires, t)) return false;
    return t > std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
}

/// 距过期剩余秒数 (供 Redis TTL; 已过期/非法返回 0)
inline int subTtlSeconds(const std::string &expires) {
    std::time_t t;
    if (!parseExpires(expires, t)) return 0;
    long long d = (long long)t -
                  (long long)std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    return d > 0 ? (int)d : 0;
}

} // namespace TimeUtils
