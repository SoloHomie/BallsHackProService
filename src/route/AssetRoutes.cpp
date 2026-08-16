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

// ── 检查用户订阅状态 (接受已认证的 TokenPayload，避免重复认证) ──
static bool checkSubscribed(AuthService &auth, const std::optional<TokenPayload> &tp,
                            std::string &subExpires) {
    if (!tp) return false;
    auto user = auth.userStore().findByEmail(tp->email);
    if (!user) return false;
    if (user->subscription_expires.empty()) return false;
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

    // ═══════════════════════════════════════════════════
    //  特征码 (订阅制: 非订阅只发全局)
    // ═══════════════════════════════════════════════════
    server.route("GET", "/api/assets/signatures",
        [&](const std::string &, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");

            std::string dummy;
            bool sub = checkSubscribed(auth, tp, dummy);

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
        });

    // ═══════════════════════════════════════════════════
    //  偏移 (订阅制: 非订阅只发摇杆XY)
    // ═══════════════════════════════════════════════════
    server.route("GET", "/api/assets/offsets",
        [&](const std::string &, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");

            std::string dummy;
            bool sub = checkSubscribed(auth, tp, dummy);

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
        });

    // ═══════════════════════════════════════════════════
    //  ELF 二进制 (仅订阅用户)
    // ═══════════════════════════════════════════════════
    server.route("GET", "/api/assets/elf",
        [&](const std::string &, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");

            std::string subExpires;
            if (!checkSubscribed(auth, tp, subExpires)) {
                return AuthService::jsonErr("FORBIDDEN",
                    "此功能需要Pro订阅。如需订阅请联系开发者。");
            }

            std::ifstream f("assets/mxelf", std::ios::binary);
            if (!f) return AuthService::jsonErr("NOT_FOUND", "ELF资源未部署");

            // 读二进制内容
            std::ostringstream ss;
            ss << f.rdbuf();
            std::string raw = ss.str();

            return "{\"success\":true,\"elf_base64\":\"" + b64encode(raw) + "\"}";
        });

    // ═══════════════════════════════════════════════════
    //  美化资源清单 (需登录)
    // ═══════════════════════════════════════════════════
    server.route("GET", "/api/assets/beautify/manifest",
        [&](const std::string &, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");

            std::ifstream f("assets/beautify_manifest.json");
            if (!f) return AuthService::jsonErr("NOT_FOUND", "美化资源未部署");
            std::ostringstream ss; ss << f.rdbuf();
            return ss.str();
        });

    // ═══════════════════════════════════════════════════
    //  美化文件下载 (需登录+订阅, 内容寻址)
    // ═══════════════════════════════════════════════════
    server.route("GET", "/api/assets/beautify/file",
        [&](const std::string &, const std::string &query, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");

            std::string subExpires;
            if (!checkSubscribed(auth, tp, subExpires)) {
                return AuthService::jsonErr("FORBIDDEN",
                    "此功能需要Pro订阅。如需订阅请联系开发者。");
            }

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
        });

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
