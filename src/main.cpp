#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#endif

#include "core/server/HttpServer.h"
#include "auth/AuthService.h"
#include "core/config/ServerConfig.h"
#include "core/config/DbConfig.h"
#include "core/config/Env.h"
#include "common/JsonConfig.h"
#include "core/database/DbPool.h"
#include "core/database/DbMigration.h"
#include "email/SmtpClient.h"
#include "route/AuthRoutes.h"
#include "route/SystemRoutes.h"
#include "route/PaymentRoutes.h"
#include "route/WelfareRoutes.h"
#include "route/WalletRoutes.h"
#include "route/SyncRoutes.h"
#include "route/AssetRoutes.h"
#include "sync/SyncService.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>

int main() {
    Env::loadDotEnv();

    if (Env::get("BH_JWT_SECRET").empty()) {
        std::cerr << "FATAL: BH_JWT_SECRET is not set" << std::endl;
        return 1;
    }

    SmtpClient::init();

#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    // 1. 数据库连接池
    DbPool pool(DbConfig::HOST, DbConfig::USER(), DbConfig::PASSWORD(),
                DbConfig::DATABASE, DbConfig::PORT, DbConfig::POOL_SIZE);

    // 2. 建表
    if (!DbMigration::run(pool)) {
        std::cerr << "FATAL: Database migration failed" << std::endl;
        return 1;
    }

    // 3. 认证服务
    AuthService auth(pool);
    HttpServer server;

    // 4. 版本检查
    server.route("GET", "/api/system/version",
        [&](const std::string &, const std::string &, const std::string &) {
            std::ifstream f("version.json");
            if (!f) return std::string("{\"latest_version\":\"1.0.0\"}");
            std::ostringstream ss; ss << f.rdbuf();
            return ss.str();
        });

    // 4.5 公开下载页 & 二进制
    server.route("GET", "/api/download",
        [&](const std::string &, const std::string &, const std::string &) {
            std::ifstream f("assets/download.html");
            if (!f) return std::string("{\"success\":false,\"message\":\"download page not found\"}");
            std::ostringstream ss; ss << f.rdbuf();
            return ss.str();  // <!DOCTYPE → text/html
        });

    server.route("GET", "/api/mxelf",
        [&](const std::string &, const std::string &, const std::string &) {
            std::ifstream f("assets/mxelf", std::ios::binary);
            if (!f) return std::string("{\"success\":false,\"message\":\"file not found\"}");
            std::ostringstream ss; ss << f.rdbuf();
            return "BIN:" + ss.str();  // BIN: prefix → application/octet-stream
        });

    // 5. 注册路由
    registerAuthRoutes(server, auth, pool);
    registerSystemRoutes(server, auth, pool);
    registerPaymentRoutes(server, auth, pool);
    registerWelfareRoutes(server, auth, pool);
    registerWalletRoutes(server, auth, pool);
    // 6. 同步服务 + 路由
    SyncService syncSvc(pool);
    registerSyncRoutes(server, auth, syncSvc);
    registerAssetRoutes(server, auth, pool);

    // 7. 启动
    bool useTls = Env::get("BH_TLS_DISABLE").empty();  // set to disable
    if (!server.start(ServerConfig::PORT, useTls)) {
        std::cerr << "Failed to start server" << std::endl;
        return 1;
    }

    std::cout << "BallsHackProService v1.0.0" << std::endl;
    std::cout << "  PORT: " << ServerConfig::PORT << std::endl;
    std::cout << "  MySQL: " << DbConfig::HOST << ":" << DbConfig::PORT
              << "/" << DbConfig::DATABASE << std::endl;

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(60));
        auth.codeManager().purgeExpired();
    }
}
