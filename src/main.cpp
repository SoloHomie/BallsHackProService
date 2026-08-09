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
#include "route/PeerRoutes.h"
#include "sync/SyncService.h"
#include "core/cache/RedisClient.h"
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

    server.route("GET", "/api/mxelf",
        [&](const std::string &, const std::string &, const std::string &) {
            std::ifstream f("assets/mxelf", std::ios::binary);
            if (!f) return std::string("{\"success\":false,\"message\":\"file not found\"}");
            std::ostringstream ss; ss << f.rdbuf();
            return "BIN:" + ss.str();
        });

    server.route("GET", "/api/installer",
        [&](const std::string &, const std::string &, const std::string &) {
            std::ifstream f("assets/BallsHackPro.zip", std::ios::binary);
            if (!f) return std::string("{\"success\":false,\"message\":\"installer not found\"}");
            std::ostringstream ss; ss << f.rdbuf();
            return "BIN:" + ss.str();
        });

    // 5. 注册路由
    registerAuthRoutes(server, auth, pool);
    registerSystemRoutes(server, auth, pool);
    registerPaymentRoutes(server, auth, pool);
    registerWelfareRoutes(server, auth, pool);
    registerWalletRoutes(server, auth, pool);
    registerPeerRoutes(server, auth, pool);
    // 6. 同步服务 + 路由
    SyncService syncSvc(pool);
    syncSvc.recoverFromDb();  // 服务重启后恢复房间内存状态
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

        // 清理断线用户的房间成员
        {
            auto guard = pool.acquire(5000);
            if (guard) {
                MYSQL *db = guard->get();
                if (mysql_query(db,
                    "SELECT r.room_code, r.controller_email, r.controlled_email, "
                    "u1.id AS ctrl_id, u2.id AS ctrld_id "
                    "FROM rooms r "
                    "LEFT JOIN users u1 ON r.controller_email = u1.email "
                    "LEFT JOIN users u2 ON r.controlled_email = u2.email "
                    "WHERE r.status != 2") == 0) {
                    MYSQL_RES *res = mysql_store_result(db);
                    if (res) {
                        MYSQL_ROW row;
                        while ((row = mysql_fetch_row(res))) {
                            std::string code(row[0] ? row[0] : "");
                            std::string ctrlUid(row[3] ? row[3] : "");
                            std::string ctrldUid(row[4] ? row[4] : "");
                            if (code.empty()) continue;

                            bool ctrlOffline = false, ctrldOffline = false;
                            try {
                                ctrlOffline = !ctrlUid.empty() &&
                                    !RedisClient::instance().exists("online:" + ctrlUid);
                                ctrldOffline = !ctrldUid.empty() &&
                                    !RedisClient::instance().exists("online:" + ctrldUid);
                            } catch (...) { continue; }

                            if (ctrlOffline) {
                                // 控制器离线 → 关闭整个房间（没人能控制）
                                syncSvc.closeRoom(code);
                                std::cout << "[Cleanup] Room " << code
                                          << " closed (controller offline)" << std::endl;
                            } else if (ctrldOffline) {
                                // 受控方离线 → 只清掉 controlled_email，房间仍可用
                                char q[256];
                                snprintf(q, sizeof(q),
                                    "UPDATE rooms SET controlled_email=NULL,status=0 "
                                    "WHERE room_code='%s'",
                                    code.c_str());
                                mysql_query(db, q);
                                std::cout << "[Cleanup] Room " << code
                                          << " (controlled left)" << std::endl;
                            }
                        }
                        mysql_free_result(res);
                    }
                }
            }
        }
    }
}
