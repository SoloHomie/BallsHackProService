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
#include "route/AdminRoutes.h"
#include "route/UpdateRoutes.h"
#include "route/PaymentRoutes.h"
#include "route/WelfareRoutes.h"
#include "route/RedeemRoutes.h"
#include "route/WalletRoutes.h"
#include "route/SyncRoutes.h"
#include "route/AssetRoutes.h"
#include "route/PeerRoutes.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>

// graceful shutdown: docker stop sends SIGTERM, exit immediately instead
// of being SIGKILLed after 10s (that window = login Connection refused)
static std::atomic<bool> g_shutdown{false};
static void onShutdownSignal(int) { g_shutdown.store(true, std::memory_order_relaxed); }

int main() {
    Env::loadDotEnv();

    if (Env::get("BH_JWT_SECRET").empty()) {
        std::cerr << "FATAL: BH_JWT_SECRET is not set" << std::endl;
        return 1;
    }

    SmtpClient::init();
    std::signal(SIGTERM, onShutdownSignal);
    std::signal(SIGINT,  onShutdownSignal);

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

    // 4. 注册路由 (内联 handler 已迁至 UpdateRoutes/AssetRoutes, 2026-09-09)
    registerAuthRoutes(server, auth, pool);
    registerSystemRoutes(server, auth);
    registerAdminRoutes(server, auth, pool);
    registerUpdateRoutes(server);
    registerPaymentRoutes(server, auth, pool);
    registerWelfareRoutes(server, auth, pool);
    registerRedeemRoutes(server, auth, pool);
    registerWalletRoutes(server, auth, pool);
    registerPeerRoutes(server, auth, pool);
    // 5. 资产路由 (sync/peer 房间统一走 PeerStore Redis, 2026-09-10)
    registerSyncRoutes(server, auth);
    registerAssetRoutes(server, auth, pool);

    // 6. 启动
    bool useTls = Env::get("BH_TLS_DISABLE").empty();  // set to disable
    if (!server.start(ServerConfig::PORT, useTls)) {
        std::cerr << "Failed to start server" << std::endl;
        return 1;
    }

    std::cout << "BallsHackProService v1.0.0" << std::endl;
    std::cout << "  PORT: " << ServerConfig::PORT << std::endl;
    std::cout << "  MySQL: " << DbConfig::HOST << ":" << DbConfig::PORT
              << "/" << DbConfig::DATABASE << std::endl;

    int tick = 0;
    while (!g_shutdown.load(std::memory_order_relaxed)) {
        // 1s slices so a signal exits within ~1s (not up to 60s)
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (++tick < 60) continue;
        tick = 0;
        auth.codeManager().purgeExpired();

        // 支付对账: 扫描未支付订单向平台查单补账 (付了钱必发货)
        paymentReconcileSweep();
        // 房间清理不再需要: Redis TTL 自过期 (2026-09-10 合并后 RoomCleaner 删除)
    }

    server.stop();
    std::cout << "Shutting down cleanly" << std::endl;
    return 0;
}
