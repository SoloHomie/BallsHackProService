#include "AssetRoutes.h"
#include "../core/server/HttpServer.h"
#include "../auth/AuthService.h"
#include "../auth/TokenService.h"
#include "../core/database/DbPool.h"
#include "../common/StringUtils.h"
#include <fstream>
#include <sstream>
#include <chrono>
#include <ctime>
#include <iomanip>

// ── 检查用户订阅状态 ──
static bool checkSubscribed(AuthService &auth, DbPool &pool, const std::string &headers,
                            std::string &subExpires) {
    auto tp = auth.authenticate(headers);
    if (!tp) return false;
    auto user = auth.userStore().findByEmail(tp->email);
    if (!user || user->subscription_expires.empty()) return false;
    subExpires = user->subscription_expires;
    auto now = std::chrono::system_clock::now();
    auto nowT = std::chrono::system_clock::to_time_t(now);
    struct tm expireTm = {};
    std::istringstream ss(subExpires);
    ss >> std::get_time(&expireTm, "%Y-%m-%d %H:%M:%S");
    auto expireT = std::mktime(&expireTm);
    return (expireT > nowT);
}

// ── JSON 转义 ──
static std::string esc(const std::string &s) {
    return AuthService::jsonEscape(s);
}

void registerAssetRoutes(HttpServer &server, AuthService &auth, DbPool &pool) {

    // ═══════════════════════════════════════════════════
    //  特征码 (订阅制: 非订阅只发全局)
    // ═══════════════════════════════════════════════════
    server.route("GET", "/api/assets/signatures",
        [&](const std::string &, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");

            bool sub = false;
            { std::string dummy; sub = checkSubscribed(auth, pool, headers, dummy); }

            std::ifstream f("assets/signatures.json");
            if (!f) return AuthService::jsonErr("NOT_FOUND", "特征码资源未部署");

            if (sub) {
                // 订阅: 返回全部特征码
                std::ostringstream ss; ss << f.rdbuf();
                return ss.str();
            }

            // 非订阅: 仅全局特征码 (只含摇杆XY+全局变速偏移)
            return std::string("{\"success\":true,\"limited\":true,\"groups\":{"
                   "\"全局\":{"
                   "\"pattern\":\"D8 00 00 00 63 04 00 00\","
                   "\"offsets\":{"
                   "\"摇杆方向X\":68,\"摇杆方向Y\":72,\"全局加速\":972"
                   "}}}}");
        });

    // ═══════════════════════════════════════════════════
    //  偏移 (订阅制: 非订阅只发摇杆XY)
    // ═══════════════════════════════════════════════════
    server.route("GET", "/api/assets/offsets",
        [&](const std::string &, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");

            bool sub = false;
            { std::string dummy; sub = checkSubscribed(auth, pool, headers, dummy); }

            std::ifstream f("assets/offsets.json");
            if (!f) return AuthService::jsonErr("NOT_FOUND", "偏移资源未部署");

            if (sub) {
                std::ostringstream ss; ss << f.rdbuf();
                return ss.str();
            }

            // 非订阅: 仅摇杆XY+全局变速
            return std::string("{\"success\":true,\"limited\":true,\"offsets\":{"
                   "\"ygfangxiang\":68,\"joystickDirY\":72,\"全局加速\":972"
                   "}}");
        });

    // ═══════════════════════════════════════════════════
    //  ELF 二进制 (仅订阅用户)
    // ═══════════════════════════════════════════════════
    server.route("GET", "/api/assets/elf",
        [&](const std::string &, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");

            std::string subExpires;
            if (!checkSubscribed(auth, pool, headers, subExpires)) {
                return AuthService::jsonErr("FORBIDDEN",
                    "此功能需要Pro订阅。如需订阅请联系开发者。");
            }

            std::ifstream f("assets/mxelf", std::ios::binary);
            if (!f) return AuthService::jsonErr("NOT_FOUND", "ELF资源未部署");

            // 读二进制内容
            std::ostringstream ss;
            ss << f.rdbuf();
            std::string raw = ss.str();

            // base64 编码 (简单实现)
            static const char *b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string b64str;
            int val = 0, valb = -6;
            for (unsigned char c : raw) {
                val = (val << 8) + c;
                valb += 8;
                while (valb >= 0) { b64str += b64[(val >> valb) & 0x3F]; valb -= 6; }
            }
            if (valb > -6) b64str += b64[((val << 8) >> (valb + 8)) & 0x3F];
            while (b64str.size() % 4) b64str += '=';

            return "{\"success\":true,\"elf_base64\":\"" + b64str + "\"}";
        });
}
