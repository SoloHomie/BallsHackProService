#include "AssetRoutes.h"
#include "RouteGuard.h"
#include "../core/server/HttpServer.h"
#include "../auth/AuthService.h"
#include "../core/database/DbPool.h"
#include "../common/StringUtils.h"
#include <fstream>
#include <sstream>
#include <chrono>
#include <ctime>
#include <iomanip>

// ── base64 编码 ──
static std::string b64encode(const std::string &raw) {
    static const char *b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for (unsigned char c : raw) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) { out += b64[(val >> valb) & 0x3F]; valb -= 6; }
    }
    if (valb > -6) out += b64[((val << 8) >> (valb + 8)) & 0x3F];
    while (out.size() % 4) out += '=';
    return out;
}

void registerAssetRoutes(HttpServer &server, AuthService &auth, DbPool &pool) {
    (void)pool;  // 资源路由只读文件 + 订阅判定, 不直连 DbPool

    // ── 游戏 ELF 载荷 (2026-09-09 自 main.cpp 迁入) ──
    server.route("GET", "/api/mxelf",
        [](const std::string &, const std::string &, const std::string &) {
            std::ifstream f("assets/mxelf", std::ios::binary);
            if (!f) return std::string("{\"success\":false,\"message\":\"file not found\"}");
            std::ostringstream ss; ss << f.rdbuf();
            return "BIN:" + ss.str();
        });

    // ── 安装包 zip (2026-09-09 自 main.cpp 迁入) ──
    server.route("GET", "/api/installer",
        [](const std::string &, const std::string &, const std::string &) {
            std::ifstream f("assets/BallsHackPro.zip", std::ios::binary);
            if (!f) return std::string("{\"success\":false,\"message\":\"installer not found\"}");
            std::ostringstream ss; ss << f.rdbuf();
            return "BIN:" + ss.str();
        });


    // ═══════════════════════════════════════════════════
    //  特征码 (订阅制: 非订阅只发全局)
    // ═══════════════════════════════════════════════════
    server.route("GET", "/api/assets/signatures",
        [&](const std::string &, const std::string &, const std::string &) {
            std::string dummy;
            bool sub = route::isSubscribed(auth, dummy);

            std::ifstream f("assets/signatures.json");
            if (!f) return AuthService::jsonErr("NOT_FOUND", "特征码资源未部署");

            if (sub) {
                // 订阅: 返回全部特征码
                std::ostringstream ss; ss << f.rdbuf();
                return ss.str();
            }

            // 非订阅: 仅全局特征码 (摇杆XY+体重+全局加速)
            return std::string("{\"success\":true,\"limited\":true,\"groups\":{"
                   "\"全局\":{"
                   "\"pattern\":\"D8 00 00 00 63 04 00 00\","
                   "\"offsets\":{"
                   "\"偏移_摇杆方向X\":68,\"偏移_摇杆方向Y\":72,\"偏移_体重\":972,\"偏移_全局加速\":972"
                   "}}}}");
        }, route::requireAuth(auth));

    // ═══════════════════════════════════════════════════
    //  偏移 (订阅制: 非订阅只发摇杆XY)
    // ═══════════════════════════════════════════════════
    server.route("GET", "/api/assets/offsets",
        [&](const std::string &, const std::string &, const std::string &) {
            std::string dummy;
            bool sub = route::isSubscribed(auth, dummy);

            std::ifstream f("assets/offsets.json");
            if (!f) return AuthService::jsonErr("NOT_FOUND", "偏移资源未部署");

            if (sub) {
                std::ostringstream ss; ss << f.rdbuf();
                return ss.str();
            }

            // 非订阅: 摇杆XY+体重+全局变速
            return std::string("{\"success\":true,\"limited\":true,\"offsets\":{"
                   "\"ygfangxiang\":68,\"joystickDirY\":72,\"tiji\":972,\"全局加速\":972"
                   "}}");
        }, route::requireAuth(auth));

    // ═══════════════════════════════════════════════════
    //  ELF 二进制 (仅订阅用户)
    // ═══════════════════════════════════════════════════
    server.route("GET", "/api/assets/elf",
        [&](const std::string &, const std::string &, const std::string &) {
            std::ifstream f("assets/mxelf", std::ios::binary);
            if (!f) return AuthService::jsonErr("NOT_FOUND", "ELF资源未部署");

            // 读二进制内容
            std::ostringstream ss;
            ss << f.rdbuf();
            std::string raw = ss.str();

            return "{\"success\":true,\"elf_base64\":\"" + b64encode(raw) + "\"}";
        }, route::requireSubscribed(auth));

    // ═══════════════════════════════════════════════════
    //  美化资源清单 (需登录)
    // ═══════════════════════════════════════════════════
    server.route("GET", "/api/assets/beautify/manifest",
        [&](const std::string &, const std::string &, const std::string &) {
            std::ifstream f("assets/beautify_manifest.json");
            if (!f) return AuthService::jsonErr("NOT_FOUND", "美化资源未部署");
            std::ostringstream ss; ss << f.rdbuf();
            return ss.str();
        }, route::requireAuth(auth));

    // ═══════════════════════════════════════════════════
    //  美化文件下载 (需登录+订阅, 内容寻址)
    // ═══════════════════════════════════════════════════
    server.route("GET", "/api/assets/beautify/file",
        [&](const std::string &, const std::string &query, const std::string &) {
            std::string path = StringUtils::queryVal(query, "path");
            // 路径校验: 只允许 beautify/ 下相对路径, 禁 .. 与绝对路径/反斜杠
            if (path.empty() || path.find("..") != std::string::npos ||
                path[0] == '/' || path[0] == '\\' ||
                path.find('\\') != std::string::npos) {
                return AuthService::jsonErr("BAD_REQUEST", "非法路径");
            }

            std::ifstream f("assets/beautify/" + path, std::ios::binary);
            if (!f) return AuthService::jsonErr("NOT_FOUND", "资源不存在");

            std::ostringstream ss; ss << f.rdbuf();
            std::string raw = ss.str();
            return "{\"success\":true,\"data_base64\":\"" + b64encode(raw) + "\"}";
        }, route::requireSubscribed(auth));

    // ═══════════════════════════════════════════════════
    //  美化预览图 (公开, 仅 png 缩略图, 供 QML Image 直接加载)
    // ═══════════════════════════════════════════════════
    server.route("GET", "/api/assets/beautify/preview",
        [&](const std::string &, const std::string &query, const std::string &) {
            std::string path = StringUtils::queryVal(query, "path");
            if (path.empty() || path.find("..") != std::string::npos ||
                path[0] == '/' || path[0] == '\\' ||
                path.find('\\') != std::string::npos ||
                path.find(".png") == std::string::npos) {
                return AuthService::jsonErr("BAD_REQUEST", "非法路径");
            }
            std::ifstream f("assets/beautify/" + path, std::ios::binary);
            if (!f) return std::string("{\"success\":false,\"message\":\"not found\"}");
            std::ostringstream ss; ss << f.rdbuf();
            return "BIN:" + ss.str();
        });
}
