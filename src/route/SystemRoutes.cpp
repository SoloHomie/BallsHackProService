#include "SystemRoutes.h"
#include "RouteGuard.h"
#include "../core/server/HttpServer.h"
#include "../auth/AuthService.h"
#include "../core/config/Env.h"

#include <fstream>
#include <sstream>
#include <iostream>

// 系统杂项路由 (2026-09-09 拆分: admin 组→AdminRoutes, 更新组→UpdateRoutes):
//   /api/payload/key   载荷密钥下发 (订阅+设备风控门控)
//   /api/announcement  系统公告
//   /api/guide         引导弹窗配置
//   /                  下载页
//   /api/ping          网络延迟探测
//   /api/health        健康检查
void registerSystemRoutes(HttpServer &server, AuthService &auth) {

    // ── 载荷密钥下发 (仅订阅用户, 2026-08-29 密钥服务器化) ──────
    server.route("GET", "/api/payload/key",
        [&](const std::string &, const std::string &, const std::string &headers) {
            const auto &tp = route::currentUser();

            // ── 设备风控: 每账号 7 天内最多 8 台设备 (防批量养号拿密钥, 2026-08-29) ──
            std::string devId;
            {
                std::string k = "X-Dev-Id:";
                auto pos = headers.find(k);
                if (pos == std::string::npos) { k = "x-dev-id:"; pos = headers.find(k); }
                if (pos != std::string::npos) {
                    pos += k.size();
                    while (pos < headers.size() && (headers[pos] == 9 || headers[pos] == 32)) pos++;
                    while (pos < headers.size() && headers[pos] != 13 && headers[pos] != 10)
                        devId += headers[pos++];
                }
            }
            // 缺/异常指纹 → 拒绝 (正常客户端必带; 裸 curl 直接拿不到密钥)
            if (devId.size() < 32 || devId.size() > 128)
                return AuthService::jsonErr("FORBIDDEN", "设备指纹缺失, 请更新客户端");

            // 设备风控规则: 7 天内 >8 台拒绝 (SQL 在 UserStore)
            {
                uint64_t uid = std::stoull(tp.sub);
                if (auth.userStore().deviceBindCount(uid) > 8)
                    return AuthService::jsonErr("FORBIDDEN", "设备数量超限, 请清理后重试");
                auth.userStore().deviceBindTouch(uid, devId);
            }

            std::string key = Env::get("BH_PAYLOAD_KEY", "");
            if (key.empty())
                return AuthService::jsonErr("SERVER_ERROR", "载荷密钥未配置");

            return "{\"success\":true,\"key\":\"" + key + "\"}";

        }, route::requireSubscribed(auth));

    // ── 系统公告 ──────────────────────────────────
    server.route("GET", "/api/announcement",
        [](const std::string &, const std::string &, const std::string &) {
            std::ifstream af;
            std::string dir = Env::exeDir();
            for (int up = 0; up <= 2; ++up) {
                af.open(dir + "/announcement.json");
                if (af) break;
                auto sep = dir.rfind('/');
                if (sep == std::string::npos) sep = dir.rfind('\\');
                if (sep == std::string::npos) break;
                dir = dir.substr(0, sep);
            }
            if (!af) return std::string("{\"active\":false}");
            return std::string((std::istreambuf_iterator<char>(af)), std::istreambuf_iterator<char>());
        });

    // ── 用户引导配置 (guide.json, bind mount) ──
    server.route("GET", "/api/guide",
        [](const std::string &, const std::string &, const std::string &) {
            std::ifstream gf;
            std::string dir = Env::exeDir();
            for (int up = 0; up <= 2; ++up) {
                gf.open(dir + "/guide.json");
                if (gf) break;
                auto sep = dir.rfind('/');
                if (sep == std::string::npos) sep = dir.rfind('\\');
                if (sep == std::string::npos) break;
                dir = dir.substr(0, sep);
            }
            if (!gf) return std::string("{\"enabled\":false}");
            return std::string((std::istreambuf_iterator<char>(gf)), std::istreambuf_iterator<char>());
        });

    // ── 下载页 ────────────────────────────────────
    server.route("GET", "/",
        [](const std::string &, const std::string &, const std::string &) {
            std::ifstream f("assets/download.html");
            if (!f) return std::string("download page not found");
            std::ostringstream ss; ss << f.rdbuf();
            return ss.str();
        });

    // ── 网络延迟探测 (零处理, 纯网络耗时) ────────
    server.route("GET", "/api/ping",
        [](const std::string &, const std::string &, const std::string &) {
            return "{\"pong\":true}";
        });

    // ── 健康检查 ──────────────────────────────────
    server.route("GET", "/api/health",
        [&](const std::string &, const std::string &, const std::string &) {
            return "{\"success\":true,\"message\":\"ok\",\"users\":" +
                   std::to_string(auth.userStore().count()) + "}";
        });
}
