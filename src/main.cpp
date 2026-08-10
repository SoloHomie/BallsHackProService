#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#endif

#include "core/server/HttpServer.h"
#include "auth/AuthService.h"
#include "auth/SessionManager.h"
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
#include <cstring>

// ── STUN-like UDP 地址发现 ─────────────────────────────
// 客户端发 UDP 包 → 服务端回复真实 NAT 后的公网 ip:port
static void stunUdpLoop() {
    int sock = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        std::cerr << "[STUN] socket() failed" << std::endl;
        return;
    }
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(8921);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[STUN] bind(:8921) failed" << std::endl;
        close(sock);
        return;
    }
    std::cout << "[STUN] UDP discovery listening on :8921" << std::endl;
    char buf[64];
    while (true) {
        sockaddr_in from{};
        socklen_t fromLen = sizeof(from);
        int n = (int)recvfrom(sock, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fromLen);
        if (n <= 0) continue;
        char ipStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &from.sin_addr, ipStr, sizeof(ipStr));
        int port = ntohs(from.sin_port);
        char resp[128];
        int respLen = snprintf(resp, sizeof(resp),
            "{\"public_ip\":\"%s\",\"public_port\":%d}", ipStr, port);
        sendto(sock, resp, respLen, 0, (sockaddr*)&from, fromLen);
    }
}

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

    // 7. STUN UDP 地址发现（端口 8921）
    std::thread(stunUdpLoop).detach();

    // 8. 启动 HTTP
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
        SessionManager::purgeExpired(&pool);       // 清理过期 refresh token
        syncSvc.purgeIdleRooms(600);               // 清理 10min 未活跃房间

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
                                // 受控方离线 → 清掉 controlled_email，房间仍可用
                                char q[256];
                                snprintf(q, sizeof(q),
                                    "UPDATE rooms SET controlled_email=NULL,status=0 "
                                    "WHERE room_code='%s'",
                                    code.c_str());
                                mysql_query(db, q);
                                // 同步清理内存状态，避免 m_rooms 积压
                                auto state = syncSvc.getOrCreateState(code);
                                if (!state->controlledEmail.empty()) {
                                    state->controlledEmail.clear();
                                    state->cv.notify_all();
                                }
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
