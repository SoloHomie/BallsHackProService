#pragma once
// ═══════════════════════════════════════════════════════════════
//  RouteGuard — 统一鉴权守卫 (2026-09-09 收敛 42 处鉴权样板)
// ═══════════════════════════════════════════════════════════════
// 机制: 注册路由时把 guard 作为 server.route() 第 4 参传入,
//   HttpServer 在 handler 执行前统一拦截 (失败按 422 发送错误体)。
// 通过时把 TokenPayload 存入线程局部上下文, handler 内用
//   route::currentUser() 获取 (替代手写 authenticate 样板)。
// 前提: HttpServer 每连接一线程, thread_local 即每请求上下文。
// 特例: heartbeat 需区分 被踢/被封/顶号, 不走本机制 (见 AuthRoutes)。
//
// 用法:
//   server.route("GET", "/api/x", handler, route::requireAuth(auth));      // 登录即可
//   server.route("POST", "/api/admin/x", h, route::requireAdmin(auth));    // 管理员
//   server.route("GET", "/api/assets/elf", h, route::requireSubscribed(auth)); // 登录+订阅
// ═══════════════════════════════════════════════════════════════

#include "../core/server/HttpServer.h"
#include "../auth/AuthService.h"
#include "../auth/TokenService.h"
#include "../common/TimeUtils.h"
#include <optional>
#include <string>

namespace route {

// ── 管理员邮箱 (收敛 SystemRoutes 13 处硬编码) ──
inline const char *kAdminEmail = "admin@solohomie.top";

// ── 线程局部: 当前请求的已认证用户 (guard 通过后 handler 内可用) ──
inline thread_local TokenPayload g_user;

// 当前请求用户; 必须在挂 requireAuth/requireAdmin/requireSubscribed 的路由上调用
inline const TokenPayload &currentUser() { return g_user; }

// ── 订阅判定唯一实现 (2026-09-09 收敛 AssetRoutes/SystemRoutes 两份副本) ──
// 规则: subscription_expires 非空且未过期 (判定统一走 TimeUtils::subActive)
inline bool isSubscribed(AuthService &auth, std::string &subExpires) {
    auto user = auth.userStore().findByEmail(g_user.email);
    if (!user || user->subscription_expires.empty()) return false;
    subExpires = user->subscription_expires;
    return TimeUtils::subActive(subExpires);
}

// ── 登录鉴权: 未登录 → 422 UNAUTHORIZED ──
inline HttpServer::AuthFn requireAuth(AuthService &auth) {
    return [&auth](const std::string &headers) -> std::optional<std::string> {
        auto tp = auth.authenticate(headers);
        if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
        g_user = *tp;
        return std::nullopt;
    };
}

// ── 管理员鉴权: 登录 + 管理员邮箱 → 否则 422 ──
inline HttpServer::AuthFn requireAdmin(AuthService &auth) {
    return [&auth](const std::string &headers) -> std::optional<std::string> {
        auto tp = auth.authenticate(headers);
        if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
        if (tp->email != kAdminEmail)
            return AuthService::jsonErr("FORBIDDEN", "无管理员权限");
        g_user = *tp;
        return std::nullopt;
    };
}

// ── 订阅鉴权: 登录 + 订阅未过期 → 否则 422 (载荷密钥/ELF/美化文件) ──
inline HttpServer::AuthFn requireSubscribed(AuthService &auth) {
    return [&auth](const std::string &headers) -> std::optional<std::string> {
        auto tp = auth.authenticate(headers);
        if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
        g_user = *tp;   // 先写入上下文, isSubscribed 依赖 g_user.email
        std::string subExpires;
        if (!isSubscribed(auth, subExpires))
            return AuthService::jsonErr("FORBIDDEN",
                "此功能需要Pro订阅。如需订阅请联系开发者。");
        return std::nullopt;
    };
}

} // namespace route
