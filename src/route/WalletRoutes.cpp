#include "WalletRoutes.h"

#include "../core/server/HttpServer.h"
#include "../auth/AuthService.h"
#include <string>

// 2026-09-06 重构: 余额 SQL 收敛到 UserStore::balanceOf,
// 本文件只做 鉴权 → 调 Store → 组 JSON。
void registerWalletRoutes(HttpServer &server, AuthService &auth, DbPool &pool) {
    (void)pool;  // 余额走 auth.userStore(), 不再直连 DbPool

    // ── 钱包余额 ──────────────────────────────────
    server.route("GET", "/api/wallet/balance",
        [&](const std::string &, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
            unsigned long long bal = auth.userStore().balanceOf(std::stoull(tp->sub));
            return "{\"success\":true,\"balance\":" + std::to_string(bal) + "}";
        });
}
