#include "WalletRoutes.h"

#include "../core/server/HttpServer.h"
#include "../auth/AuthService.h"
#include "../common/StringUtils.h"
#include "../core/database/DbPool.h"

#include <cstring>
#include <mysql/mysql.h>
#include <string>

void registerWalletRoutes(HttpServer &server, AuthService &auth, DbPool &pool) {

    // ── 钱包余额 ──────────────────────────────────
    server.route("GET", "/api/wallet/balance",
        [&](const std::string &, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
            uint64_t uid = std::stoull(tp->sub);

            auto guard = pool.acquire(5000);
            if (!guard) return AuthService::jsonErr("DB_ERROR", "数据库繁忙，请稍后重试");
            MYSQL* db = guard->get();
            MYSQL_STMT* stmt = mysql_stmt_init(db);
            const char* sql = "SELECT balance FROM users WHERE id=?";
            mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql));
            MYSQL_BIND b[1] = {};
            b[0].buffer_type = MYSQL_TYPE_LONGLONG; b[0].buffer = &uid;
            mysql_stmt_bind_param(stmt, b);
            mysql_stmt_execute(stmt); mysql_stmt_store_result(stmt);
            unsigned long long bal = 0;
            MYSQL_BIND r[1] = {};
            r[0].buffer_type = MYSQL_TYPE_LONGLONG; r[0].buffer = &bal;
            mysql_stmt_bind_result(stmt, r);
            mysql_stmt_fetch(stmt);
            mysql_stmt_close(stmt);
            return "{\"success\":true,\"balance\":" + std::to_string(bal) + "}";
        });
}
