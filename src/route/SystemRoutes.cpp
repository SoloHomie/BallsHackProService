#include "SystemRoutes.h"
#include <iostream>
#include <exception>
// PaymentRoutes.h 提供 paymentService() 全局访问器 (admin 退款路由使用);
// VPayService.h 提供其方法调用的完整类型定义
#include "PaymentRoutes.h"
#include "../payment/VPayService.h"

#include "../core/server/HttpServer.h"
#include "../core/server/TlsConnection.h"
#include "../auth/AuthService.h"
#include "../common/StringUtils.h"
#include "../common/TimeUtils.h"
#include "../common/Snowflake.h"
#include "../core/cache/RedisClient.h"
#include "../core/database/DbPool.h"
#include "../core/config/ServerConfig.h"
#include "../core/config/Env.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mswsock.h>
#endif

#include <sstream>
#include <fstream>
#include <vector>
#include <ctime>
#include <sys/stat.h>
#include <filesystem>
#include <cstdint>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <random>
#include "../common/Snowflake.h"
#include <cstring>

// ── inline MD5 (RFC 1321) ────────────────────────
namespace {
struct MD5 {
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    uint64_t len = 0;
    uint8_t  buf[64] = {};
    size_t   bufLen = 0;

    static uint32_t rol(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }
    void transform(const uint8_t block[64]) {
        static const uint32_t K[64] = {
            0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
            0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
            0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
            0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
            0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
            0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
            0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
            0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
        };
        static const int S[64] = {
            7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
            5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
            4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
            6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
        };
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; i++) {
            uint32_t f, g;
            if (i < 16)      { f = (b & c) | (~b & d);          g = i; }
            else if (i < 32) { f = (d & b) | (~d & c);          g = (5*i+1) % 16; }
            else if (i < 48) { f = b ^ c ^ d;                   g = (3*i+5) % 16; }
            else             { f = c ^ (b | ~d);                g = (7*i) % 16; }
            uint32_t tmp = d; d = c; c = b;
            uint32_t m = (uint32_t(block[g*4]) | (uint32_t(block[g*4+1])<<8)
                         | (uint32_t(block[g*4+2])<<16) | (uint32_t(block[g*4+3])<<24));
            b = b + rol(a + f + K[i] + m, S[i]);
            a = tmp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    }
    void update(const uint8_t *data, size_t n) {
        len += n;
        size_t idx = 0;
        if (bufLen) {
            size_t fill = 64 - bufLen;
            if (n < fill) { memcpy(buf + bufLen, data, n); bufLen += n; return; }
            memcpy(buf + bufLen, data, fill);
            transform(buf); bufLen = 0; idx = fill; n -= fill;
        }
        while (n >= 64) { transform(data + idx); idx += 64; n -= 64; }
        if (n) { memcpy(buf, data + idx, n); bufLen = n; }
    }
    std::string hex() {
        uint64_t bits = len * 8;
        uint8_t pad[72] = {0x80};
        size_t padLen = (bufLen < 56) ? (56 - bufLen) : (120 - bufLen);
        for (int i = 0; i < 8; i++) pad[padLen + i] = (uint8_t)(bits >> (i * 8));
        update(pad, padLen + 8);
        char out[33];
        for (int i = 0; i < 4; i++)
            snprintf(out + i*8, 9, "%02x%02x%02x%02x",
                (uint8_t)h[i], (uint8_t)(h[i]>>8), (uint8_t)(h[i]>>16), (uint8_t)(h[i]>>24));
        return std::string(out, 32);
    }
};

std::string md5File(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    MD5 md5;
    char buf[8192];
    while (f.read(buf, sizeof(buf)) || f.gcount() > 0)
        md5.update((const uint8_t*)buf, (size_t)f.gcount());
    return md5.hex();
}

// ── 检查用户订阅状态 (与 AssetRoutes.cpp 同款, 2026-08-29 载荷密钥门控) ──
static bool checkSubscribed(AuthService &auth, const std::optional<TokenPayload> &tp,
                            std::string &subExpires) {
    if (!tp) return false;
    auto user = auth.userStore().findByEmail(tp->email);
    if (!user || user->subscription_expires.empty()) return false;
    subExpires = user->subscription_expires;
    return TimeUtils::subActive(subExpires);   // 判定统一走 TimeUtils
}
} // anonymous namespace

void registerSystemRoutes(HttpServer &server, AuthService &auth, DbPool &pool) {
    // ── 载荷密钥下发 (仅订阅用户, 2026-08-29 密钥服务器化) ──────
    server.route("GET", "/api/payload/key",
        [&](const std::string &, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");

            std::string subExpires;
            if (!checkSubscribed(auth, tp, subExpires))
                return AuthService::jsonErr("FORBIDDEN", "此功能需要Pro订阅。如需订阅请联系开发者。");

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

            {
                auto g = pool.acquire(5000);
                if (!g) return AuthService::jsonErr("DB_ERROR", "db busy");
                MYSQL *db = g->get();
                char q[1024]; char eesc[160];
                mysql_real_escape_string(db, eesc, devId.c_str(), devId.size());
                snprintf(q, sizeof(q),
                    "SELECT COUNT(*) FROM device_bind WHERE uid=%s AND first_seen > NOW() - INTERVAL 7 DAY",
                    tp->sub.c_str());
                if (mysql_query(db, q) != 0)
                    return AuthService::jsonErr("DB_ERROR", mysql_error(db));
                MYSQL_RES *res = mysql_store_result(db);
                bool over = false;
                if (res) {
                    MYSQL_ROW row = mysql_fetch_row(res);
                    if (row && row[0]) over = (strtoull(row[0], nullptr, 10) > 8);
                    mysql_free_result(res);
                }
                if (over)
                    return AuthService::jsonErr("FORBIDDEN", "设备数量超限, 请清理后重试");
                snprintf(q, sizeof(q),
                    "INSERT INTO device_bind(uid,device_id,first_seen,last_seen) VALUES(%s,'%s',NOW(),NOW()) "
                    "ON DUPLICATE KEY UPDATE last_seen=NOW()", tp->sub.c_str(), eesc);
                if (mysql_query(db, q) != 0)
                    return AuthService::jsonErr("DB_ERROR", mysql_error(db));
            }

            std::string key = Env::get("BH_PAYLOAD_KEY", "");
            if (key.empty())
                return AuthService::jsonErr("SERVER_ERROR", "载荷密钥未配置");

            return "{\"success\":true,\"key\":\"" + key + "\"}";

        });

    // ── 版本检查 / 自动更新 ────────────────────────
    server.route("GET", "/api/version",
        [&](const std::string &, const std::string &query, const std::string &) {
            std::string clientVer = StringUtils::queryVal(query, "current");
            std::string latestVer = "1.0.0", minVer = "1.0.0", changelog = "";
            std::string su = "";   // silent_update: 空串 → 默认 true (兼容无字段旧配置)

            // 读取 version.json：尝试 exe 目录 + 向上查找（兼容 VS 调试路径）
            std::ifstream vf;
            std::string dir = Env::exeDir();
            for (int up = 0; up <= 2; ++up) {
#ifdef _WIN32
                vf.open(dir + "\\version.json");
#else
                vf.open(dir + "/version.json");
#endif
                if (vf) break;
                auto sep = dir.rfind(
#ifdef _WIN32
                    '\\'
#else
                    '/'
#endif
                );
                if (sep == std::string::npos) break;
                dir = dir.substr(0, sep);
            }
            if (!vf) {
                std::cout << "[Version] NOT FOUND — using defaults" << std::endl;
            }
            if (vf) {
                std::string content((std::istreambuf_iterator<char>(vf)),
                                     std::istreambuf_iterator<char>());
                auto extract = [&](const std::string &key) -> std::string {
                    auto pos = content.find("\"" + key + "\"");
                    if (pos == std::string::npos) return "";
                    pos = content.find(':', pos + key.size() + 2);
                    if (pos == std::string::npos) return "";
                    pos = content.find('"', pos + 1);
                    if (pos == std::string::npos) return "";
                    pos++; // skip opening quote
                    auto end = content.find('"', pos);
                    if (end == std::string::npos) return "";
                    return content.substr(pos, end - pos);
                };
                std::string lv = extract("latest_version");
                std::string mv = extract("min_version");
                std::string cl = extract("changelog");
                su = extract("silent_update");
                if (!lv.empty()) latestVer = lv;
                if (!mv.empty()) minVer = mv;
                if (!cl.empty()) changelog = cl;
            }

            
            // JSON string escape helper
            auto jsonEscape = [](const std::string &s) -> std::string {
                std::string r;
                r.reserve(s.size() + 16);
                for (char c : s) {
                    switch (c) {
                        case '"':  r += "\\\""; break;
                        case '\\': r += "\\\\"; break;
                        case '\n': r += "\\n";  break;
                        case '\r': r += "\\r";  break;
                        case '\t': r += "\\t";  break;
                        default:   r += c;
                    }
                }
                return r;
            };

            // 下载地址: 从 version.json 读取, 否则用默认
            std::string directDl, cloudDl;
            if (vf) {
                vf.clear(); vf.seekg(0);
                std::string content2((std::istreambuf_iterator<char>(vf)), std::istreambuf_iterator<char>());
                auto ext = [&](const std::string &key) -> std::string {
                    auto pos = content2.find("\"" + key + "\"");
                    if (pos == std::string::npos) return "";
                    pos = content2.find(':', pos + key.size() + 2);
                    if (pos == std::string::npos) return "";
                    pos = content2.find('"', pos + 1);
                    if (pos == std::string::npos) return "";
                    pos++;
                    auto end = content2.find('"', pos);
                    if (end == std::string::npos) return "";
                    return content2.substr(pos, end - pos);
                };
                directDl = ext("direct_download_url");
                cloudDl  = ext("download_url");
            }
            if (directDl.empty()) directDl = "https://38.92.9.178/api/download";
            if (cloudDl.empty()) cloudDl  = "https://api.solohomie.top/api/download";

            // 语义化版本比较 (缺失段视为0)
            auto cmpSemVer = [](const std::string &a, const std::string &b) -> int {
                auto split = [](const std::string &v) -> std::vector<int> {
                    std::vector<int> parts;
                    std::stringstream ss(v);
                    std::string part;
                    while (std::getline(ss, part, '.')) {
                        try { parts.push_back(std::stoi(part)); }
                        catch (...) { parts.push_back(0); }
                    }
                    return parts;
                };
                auto av = split(a), bv = split(b);
                // (std::max) 加括号: 防 windows.h 的 max 宏冲突 (Linux 无此问题)
                size_t maxLen = (std::max)(av.size(), bv.size());
                av.resize(maxLen, 0);
                bv.resize(maxLen, 0);
                for (size_t i = 0; i < maxLen; ++i) {
                    if (av[i] < bv[i]) return -1;
                    if (av[i] > bv[i]) return 1;
                }
                return 0;
            };
            bool force = false;
            if (!clientVer.empty() && !minVer.empty())
                force = (cmpSemVer(clientVer, minVer) < 0);
            return std::string("{\"success\":true")
                + ",\"latest_version\":\"" + latestVer + "\""
                + ",\"min_version\":\"" + minVer + "\""
                + ",\"force_update\":" + (force ? "true" : "false")
                + ",\"update_type\":\"" + (force ? "force" : "recommended") + "\""
                + ",\"silent_update\":" + (su.empty() || su == "true" ? "true" : "false")
                + ",\"download_url\":\"" + cloudDl + "\""
                + ",\"direct_download_url\":\"" + directDl + "\""
                + ",\"changelog\":\"" + jsonEscape(changelog) + "\""
                + "}";
        });

    // ── 系统公告 ──────────────────────────────────
    server.route("GET", "/api/announcement",
        [&](const std::string &, const std::string &, const std::string &) {
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
        [&](const std::string &, const std::string &, const std::string &) {
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

    // ── 更新文件下载: /api/dl?file=xxx → releases/xxx ──
    // 更新文件下载: /api/dl?file=xxx → releases/xxx
    // GET 下载本体; HEAD 供探测(同 handler, body 客户端忽略) 2026-08-29
    auto dlHandler = [](socket_t sock, TlsConnection *tls, const std::string &query) {
            std::string file = StringUtils::queryVal(query, "file");
            if (file.empty() || file.find("..") != std::string::npos) {
                std::string err = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                if (tls) tls->write(err.data(), (int)err.size());
                else send(sock, err.data(), (int)err.size(), 0);
                return;
            }
            std::string fullPath = Env::exeDir() + "/releases/" + file;
            // 读取整个文件到内存，避免 TOCTOU 竞态
            std::ifstream fs(fullPath, std::ios::binary | std::ios::ate);
            if (!fs) {
                std::string err = "HTTP/1.1 404 Not Found\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Content-Length: 0\r\nConnection: close\r\n\r\n";
                if (tls) tls->write(err.data(), (int)err.size());
                else { const char *p = err.data(); size_t left = err.size(); while (left > 0) { auto sent = send(sock, p, (int)left, 0); if (sent <= 0) break; left -= sent; p += sent; } }
                return;
            }
            size_t fsize = fs.tellg();
            fs.seekg(0, std::ios::beg);
            std::vector<char> fileBuf(fsize);
            fs.read(fileBuf.data(), fsize);
            fs.close();
            std::ostringstream hdr;
            hdr << "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                << "Content-Length: " << fsize << "\r\n"
                << "Access-Control-Allow-Origin: *\r\n"
                << "Connection: close\r\n\r\n";
            std::string h = hdr.str();
            // 发送 header（检查返回值）
            { const char *p = h.data(); size_t left = h.size(); while (left > 0) { auto sent = tls ? tls->write(p, (int)left) : send(sock, p, (int)left, 0); if (sent <= 0) return; left -= sent; p += sent; } }
            // 发送 body
            { const char *p = fileBuf.data(); size_t left = fsize; while (left > 0) { auto sent = tls ? tls->write(p, (int)left) : send(sock, p, (int)left, 0); if (sent <= 0) break; left -= sent; p += sent; } }
    };
    server.routeStream("GET", "/api/dl", dlHandler);
    server.routeStream("HEAD", "/api/dl", dlHandler);

    // ── 下载更新包 (满带宽发送) ──
    server.routeStream("GET", "/api/download",
        [](socket_t sock, TlsConnection *tls, const std::string &) {
            std::string path = Env::exeDir() + "/releases/BallsHackPro-setup-latest.exe";
            // 读取整个文件到内存，避免 TOCTOU 竞态
            std::ifstream fs(path, std::ios::binary | std::ios::ate);
            if (!fs) {
                std::string err = "HTTP/1.1 404 Not Found\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Content-Length: 0\r\nConnection: close\r\n\r\n";
                if (tls) tls->write(err.data(), (int)err.size());
                else { const char *p = err.data(); size_t left = err.size(); while (left > 0) { auto sent = send(sock, p, (int)left, 0); if (sent <= 0) break; left -= sent; p += sent; } }
                return;
            }
            size_t fsize = fs.tellg();
            fs.seekg(0, std::ios::beg);
            std::vector<char> fileBuf(fsize);
            fs.read(fileBuf.data(), fsize);
            fs.close();
            // 从 version.json 读取版本号来生成下载文件名
            std::string ver = "";
            {
                std::ifstream vf(Env::exeDir() + "/version.json");
                if (vf) {
                    std::string j((std::istreambuf_iterator<char>(vf)),
                                   std::istreambuf_iterator<char>());
                    auto p = j.find("\"latest_version\"");
                    if (p != std::string::npos) {
                        p = j.find('"', p + 17);
                        if (p != std::string::npos) {
                            p++;
                            auto e = j.find('"', p);
                            if (e != std::string::npos) ver = j.substr(p, e - p);
                        }
                    }
                }
            }
            std::string filename = "BallsHackPro-setup" + (ver.empty() ? "" : "-v" + ver) + ".exe";
            {
                std::ostringstream hdr;
                hdr << "HTTP/1.1 200 OK\r\n"
                    << "Content-Type: application/octet-stream\r\n"
                    << "Content-Length: " << fsize << "\r\n"
                    << "Content-Disposition: attachment; filename=\"" << filename << "\"\r\n"
                    << "Cache-Control: public, max-age=86400\r\n"
                    << "Connection: close\r\n"
                    << "Access-Control-Allow-Origin: *\r\n\r\n";
                std::string h = hdr.str();
                if (tls) tls->write(h.data(), (int)h.size());
                else send(sock, h.data(), (int)h.size(), 0);
            }
            // 发送 body（检查返回值避免静默丢数据）
            { const char *p = fileBuf.data(); size_t left = fsize; while (left > 0) { auto sent = tls ? tls->write(p, (int)left) : send(sock, p, (int)left, 0); if (sent <= 0) break; left -= sent; p += sent; } }
        });

    // ── 下载页 ────────────────────────────────────
    server.route("GET", "/",
        [&](const std::string &, const std::string &, const std::string &) {
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

    // ── 管理: 修改订阅天数 (需admin token) ──────────
    server.route("POST", "/api/admin/subscription",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
            if (tp->email != "admin@solohomie.top")
                return AuthService::jsonErr("FORBIDDEN", "无管理员权限");
            std::string email = StringUtils::jsonVal(body, "email");
            int days = std::stoi(StringUtils::jsonVal(body, "days"));
            if (email.empty() || days == 0)
                return AuthService::jsonErr("FIELDS_REQUIRED", "email和days必填");
            auto g = pool.acquire(5000);
            if (!g) return AuthService::jsonErr("DB_ERROR", "db busy");
            MYSQL *db = g->get();
            char q[512]; char eesc[256];
            mysql_real_escape_string(db, eesc, email.c_str(), email.size());
            if (days > 0)
                snprintf(q, sizeof(q),
                    "UPDATE users SET subscription_expires=DATE_ADD("
                    "GREATEST(COALESCE(subscription_expires,NOW()),NOW()), INTERVAL %d DAY)"
                    " WHERE email='%s'", days, eesc);
            else
                snprintf(q, sizeof(q),
                    "UPDATE users SET subscription_expires=DATE_SUB("
                    "GREATEST(COALESCE(subscription_expires,NOW()),NOW()), INTERVAL %d DAY)"
                    " WHERE email='%s' AND subscription_expires IS NOT NULL", -days, eesc);
            if (mysql_query(db, q) != 0)
                return AuthService::jsonErr("DB_ERROR", mysql_error(db));
            if (mysql_affected_rows(db) == 0)
                return AuthService::jsonErr("NOT_FOUND", "用户不存在或未订阅");
            // 读出新过期时间，直接写回 Redis（避免 DEL+GET 竞态）
            {
                auto u = auth.userStore().findByEmail(email);
                if (u && !u->subscription_expires.empty()) {
                    int ttl = TimeUtils::subTtlSeconds(u->subscription_expires);
                    if (ttl < 60) ttl = 60;
                    try {
                        RedisClient::instance().setex(
                            "sub:" + std::to_string(u->id), ttl, u->subscription_expires);
                    } catch (const std::exception &e) {
                        std::cerr << "[SubSync] setex FAILED sub:" << u->id << ": " << e.what() << std::endl;
                    }
                }
            }
            std::cout << "[Admin] subscription email=" << email << " days=" << days << std::endl;
            return AuthService::jsonOk(days > 0
                ? "已添加 " + std::to_string(days) + " 天"
                : "已减少 " + std::to_string(-days) + " 天");
        });

    // ── 管理: 查询订阅 (需admin token) ──────────────
    server.route("GET", "/api/admin/subscription",
        [&](const std::string &, const std::string &query, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
            if (tp->email != "admin@solohomie.top")
                return AuthService::jsonErr("FORBIDDEN", "无管理员权限");
            std::string email = StringUtils::queryVal(query, "email");
            if (email.empty()) return AuthService::jsonErr("FIELDS_REQUIRED", "email必填");
            auto user = auth.userStore().findByEmail(email);
            if (!user) return AuthService::jsonErr("NOT_FOUND", "用户不存在");
            std::ostringstream resp;
            resp << "{\"success\":true"
                 << ",\"email\":\"" << AuthService::jsonEscape(user->email) << "\""
                 << ",\"username\":\"" << AuthService::jsonEscape(user->username) << "\""
                 << ",\"status\":" << user->status
                 << ",\"subscription_expires\":\"" << AuthService::jsonEscape(user->subscription_expires) << "\"";
            // 计算剩余天数
            if (!user->subscription_expires.empty()) {
                int rem = TimeUtils::subTtlSeconds(user->subscription_expires) / 86400;
                resp << ",\"remaining_days\":" << rem;
            } else { resp << ",\"remaining_days\":0"; }
            resp << "}";
            return resp.str();
        });

    // ── 管理: 在线用户列表 ─────────────────────────

    // ── 增量更新文件列表 (跨平台: std::filesystem + 内联MD5) ──
    server.route("GET", "/api/update/manifest",
        [](const std::string &, const std::string &, const std::string &) {
            namespace fs = std::filesystem;
            std::string dir = Env::exeDir() + "/releases/latest_full";
            std::ostringstream resp; resp << "[";
            bool first = true;
            try {
                for (const auto &entry : fs::recursive_directory_iterator(dir)) {
                    if (!entry.is_regular_file()) continue;
                    std::string absPath = entry.path().string();
                    std::string rel = absPath.substr(dir.size() + 1);
#ifdef _WIN32
                    for (auto &c : rel) if (c == '\\') c = '/';
#endif
                    std::string md5 = md5File(absPath);
                    if (!first) resp << ","; first = false;
                    resp << "{\"path\":\"" << rel << "\",\"md5\":\"" << md5
                         << "\",\"size\":" << entry.file_size()
                         << ",\"url\":\"/api/dl?file=latest_full/" << rel << "\"}";
                }
            } catch (...) {}
            resp << "]";
            return resp.str();
        });
    // ── 全量更新: zip 下载 ──────────────────────────
    server.routeStream("GET", "/api/update/download",
        [](socket_t sock, TlsConnection *tls, const std::string &) {
            std::string path = Env::exeDir() + "/releases/BallsHackPro-latest.zip";
            // 读取整个文件到内存，避免 TOCTOU 竞态
            std::ifstream fs(path, std::ios::binary | std::ios::ate);
            if (!fs) {
                std::string err = "HTTP/1.1 404 Not Found\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Content-Length: 0\r\nConnection: close\r\n\r\n";
                if (tls) tls->write(err.data(), (int)err.size());
                else { const char *p = err.data(); size_t left = err.size(); while (left > 0) { auto sent = send(sock, p, (int)left, 0); if (sent <= 0) break; left -= sent; p += sent; } }
                return;
            }
            size_t fsize = fs.tellg();
            fs.seekg(0, std::ios::beg);
            std::vector<char> fileBuf(fsize);
            fs.read(fileBuf.data(), fsize);
            fs.close();
            std::ostringstream hdr;
            hdr << "HTTP/1.1 200 OK\r\n"
                << "Content-Type: application/zip\r\n"
                << "Content-Length: " << fsize << "\r\n"
                << "Content-Disposition: attachment; filename=\"BallsHackPro-update.zip\"\r\n"
                << "Cache-Control: public, max-age=86400\r\n"
                << "Connection: close\r\n"
                << "Access-Control-Allow-Origin: *\r\n\r\n";
            std::string h = hdr.str();
            // 发送 header（检查返回值）
            { const char *p = h.data(); size_t left = h.size(); while (left > 0) { auto sent = tls ? tls->write(p, (int)left) : send(sock, p, (int)left, 0); if (sent <= 0) return; left -= sent; p += sent; } }
            // 发送 body
            { const char *p = fileBuf.data(); size_t left = fsize; while (left > 0) { auto sent = tls ? tls->write(p, (int)left) : send(sock, p, (int)left, 0); if (sent <= 0) break; left -= sent; p += sent; } }
        });



    server.route("GET", "/api/admin/online-users",
        [&](const std::string &, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
            if (tp->email != "admin@solohomie.top")
                return AuthService::jsonErr("FORBIDDEN", "无管理员权限");
            std::ostringstream resp;
            resp << "{\"success\":true,\"users\":[";
            int count = 0;
            try {
                auto &r = RedisClient::instance();
                std::vector<std::string> keys;
                r.keys("online:*", std::back_inserter(keys));
                for (size_t i = 0; i < keys.size(); ++i) {
                    auto email = r.get(keys[i]);
                    if (!email) continue;
                    if (count > 0) resp << ",";
                    resp << "{\"uid\":\"" << keys[i].substr(7) << "\""
                         << ",\"email\":\"" << *email << "\"}";
                    count++;
                }
            } catch (...) {}
            resp << "],\"count\":" << count << "}";
            return resp.str();
        });

    // ── 管理: 踢人下线 ─────────────────────────────
    server.route("POST", "/api/admin/kick-user",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
            if (tp->email != "admin@solohomie.top")
                return AuthService::jsonErr("FORBIDDEN", "无管理员权限");
            std::string email = StringUtils::jsonVal(body, "email");
            std::string uid   = StringUtils::jsonVal(body, "uid");
            if (email.empty() && uid.empty())
                return AuthService::jsonErr("FIELDS_REQUIRED", "email或uid必填");
            try {
                auto &r = RedisClient::instance();
                if (!uid.empty()) {
                    // 5分钟窗口: 客户端5s心跳必能收到; 覆盖短暂离线/重启场景
                    r.setex("kicked:" + uid, 300, "1");
                } else {
                    // 通过 email 找 uid
                    auto user = auth.userStore().findByEmail(email);
                    if (user) r.setex("kicked:" + std::to_string(user->id), 300, "1");
                }
            } catch (...) {}
            return AuthService::jsonOk("已踢出，用户下次心跳将被强制下线");
        });

    // ── 管理: 设置订阅到期时间 (绝对值, 覆盖式) ─────
    server.route("POST", "/api/admin/set-subscription",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
            if (tp->email != "admin@solohomie.top")
                return AuthService::jsonErr("FORBIDDEN", "无管理员权限");
            std::string email   = StringUtils::jsonVal(body, "email");
            std::string expires = StringUtils::jsonVal(body, "expires");
            if (email.empty() || expires.empty())
                return AuthService::jsonErr("FIELDS_REQUIRED", "email和expires必填");
            auto u = auth.userStore().findByEmail(email);
            if (!u) return AuthService::jsonErr("NOT_FOUND", "用户不存在");
            auto g = pool.acquire(5000);
            if (!g) return AuthService::jsonErr("DB_ERROR", "db busy");
            MYSQL *db = g->get();
            char q[512]; char eesc[256]; char eesc2[64];
            mysql_real_escape_string(db, eesc, email.c_str(), email.size());
            if (expires == "clear") {
                snprintf(q, sizeof(q),
                    "UPDATE users SET subscription_expires=NULL WHERE email='%s'", eesc);
            } else {
                // 格式: YYYY-MM-DD HH:MM:SS (或 YYYY-MM-DD)
                mysql_real_escape_string(db, eesc2, expires.c_str(), expires.size());
                snprintf(q, sizeof(q),
                    "UPDATE users SET subscription_expires='%s' WHERE email='%s'", eesc2, eesc);
            }
            if (mysql_query(db, q) != 0)
                return AuthService::jsonErr("DB_ERROR", mysql_error(db));
            // 回写 Redis 缓存 (与加天数接口同款逻辑; clear 时删缓存)
            auto nu = auth.userStore().findByEmail(email);
            if (nu) {
                try {
                    auto &r = RedisClient::instance();
                    if (nu->subscription_expires.empty()) {
                        r.del("sub:" + std::to_string(nu->id));
                    } else {
                        int ttl = TimeUtils::subTtlSeconds(nu->subscription_expires);
                        if (ttl < 60) ttl = 60;
                        r.setex("sub:" + std::to_string(nu->id), ttl, nu->subscription_expires);
                    }
                } catch (const std::exception &e) {
                    std::cerr << "[SubSync] setex FAILED sub:" << nu->id << ": " << e.what() << std::endl;
                }
            }
            std::cout << "[Admin] set-subscription email=" << email << " expires=" << expires << std::endl;
            return AuthService::jsonOk(expires == "clear"
                ? "已清除订阅到期时间"
                : "已设置到期时间: " + expires);
        });

    // ── 管理: 定向专属福利 (发到指定用户, 仅该用户可见可领; 领取=加 N 天 Pro) ──
    server.route("POST", "/api/admin/welfare-grant",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
            if (tp->email != "admin@solohomie.top")
                return AuthService::jsonErr("FORBIDDEN", "无管理员权限");
            std::string email = StringUtils::jsonVal(body, "email");
            std::string daysStr = StringUtils::jsonVal(body, "days");
            std::string name = StringUtils::jsonVal(body, "name");
            std::string desc = StringUtils::jsonVal(body, "description");
            std::string expStr = StringUtils::jsonVal(body, "expires_days");
            // 2026-08-26 修复: 空串直接 stoi 抛异常 → 500; 先判空再解析
            if (email.empty() || daysStr.empty() ||
                daysStr.find_first_not_of("0123456789") != std::string::npos)
                return AuthService::jsonErr("FIELDS_REQUIRED", "email和days必填(天数>0)");
            int days = std::stoi(daysStr);
            int expiresDays = expStr.empty() ? 0 : std::stoi(expStr);
            if (name.empty()) name = "专属福利";
            if (desc.empty()) desc = "管理员定向发放";
            if (expiresDays <= 0) expiresDays = 30;
            auto u = auth.userStore().findByEmail(email);
            if (!u) return AuthService::jsonErr("NOT_FOUND", "用户不存在");
            auto g = pool.acquire(5000);
            if (!g) return AuthService::jsonErr("DB_ERROR", "db busy");
            MYSQL *db = g->get();
            char q[1024]; char eesc[256]; char neesc[128]; char descsc[512];
            mysql_real_escape_string(db, eesc, email.c_str(), email.size());
            mysql_real_escape_string(db, neesc, name.c_str(), name.size());
            mysql_real_escape_string(db, descsc, desc.c_str(), desc.size());
            uint64_t eid = Snowflake::generate();
            snprintf(q, sizeof(q),
                "INSERT INTO welfare_events (id, name, description, event_type, target_email,"
                " reward_amount, starts_at, expires_at, is_active)"
                " VALUES (%llu, '%s', '%s', 'general', '%s', %d,"
                " NOW(), DATE_ADD(NOW(), INTERVAL %d DAY), 1)",
                eid, neesc, descsc, eesc, days, expiresDays);
            if (mysql_query(db, q) != 0)
                return AuthService::jsonErr("DB_ERROR", mysql_error(db));
            return AuthService::jsonOk("已向 " + email + " 发放专属福利: " + name
                + " (+" + std::to_string(days) + "天 Pro, " + std::to_string(expiresDays) + " 天内可领)");
        });

    // ── 管理: 按累计充值金额定向发福利 (2026-08-26) ──
    // 筛选 paid 订单聚合 ≥ min_amount 元的用户, 每人插一条定向 welfare_events
    // 幂等: 同用户+同福利名且未过期 → 跳过 (防 bot 重复执行重复发)
    server.route("POST", "/api/admin/welfare-grant-by-amount",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
            if (tp->email != "admin@solohomie.top")
                return AuthService::jsonErr("FORBIDDEN", "无管理员权限");
            std::string minStr = StringUtils::jsonVal(body, "min_amount");
            std::string daysStr = StringUtils::jsonVal(body, "days");
            std::string name = StringUtils::jsonVal(body, "name");
            std::string desc = StringUtils::jsonVal(body, "description");
            std::string expStr = StringUtils::jsonVal(body, "expires_days");
            // 先判空再解析 (2026-08-26 修复风格: 空串 stoi 抛异常 → 500)
            if (minStr.empty() || daysStr.empty() ||
                minStr.find_first_not_of("0123456789") != std::string::npos ||
                daysStr.find_first_not_of("0123456789") != std::string::npos)
                return AuthService::jsonErr("FIELDS_REQUIRED", "min_amount(元)和days必填(正整数)");
            long long minYuan = std::stoll(minStr);
            int days = std::stoi(daysStr);
            int expiresDays = expStr.empty() ? 0 : std::stoi(expStr);
            if (name.empty()) name = "用户反馈礼包";
            if (desc.empty()) desc = "感谢您的充值支持！请填写反馈问卷，福利已发放";
            if (expiresDays <= 0) expiresDays = 30;
            if (minYuan <= 0 || days <= 0)
                return AuthService::jsonErr("FIELDS_REQUIRED", "min_amount和days须为正数");
            auto g = pool.acquire(5000);
            if (!g) return AuthService::jsonErr("DB_ERROR", "db busy");
            MYSQL *db = g->get();
            // 1) 累计充值 ≥ 门槛的用户 (amount 单位=分)
            char q[2048];
            snprintf(q, sizeof(q),
                "SELECT u.id, u.email, SUM(p.amount) FROM payment_orders p "
                "JOIN users u ON u.id=p.user_id WHERE p.status='paid' "
                "GROUP BY u.id HAVING SUM(p.amount) >= %lld", minYuan * 100);
            if (mysql_query(db, q) != 0)
                return AuthService::jsonErr("DB_ERROR", mysql_error(db));
            MYSQL_RES *res = mysql_store_result(db);
            if (!res) return AuthService::jsonErr("DB_ERROR", "no result");
            MYSQL_ROW row;
            int granted = 0, skipped = 0;
            char neesc[128]; char descsc[512]; char eesc[256];
            while ((row = mysql_fetch_row(res)) != nullptr) {
                uint64_t uid = strtoull(row[0], nullptr, 10);
                std::string email = row[1] ? row[1] : "";
                if (email.empty()) { skipped++; continue; }
                mysql_real_escape_string(db, eesc, email.c_str(), email.size());
                mysql_real_escape_string(db, neesc, name.c_str(), name.size());
                mysql_real_escape_string(db, descsc, desc.c_str(), desc.size());
                // 2) 幂等: 同用户同福利名未过期 → 跳过
                snprintf(q, sizeof(q),
                    "SELECT COUNT(*) FROM welfare_events WHERE target_email='%s' "
                    "AND name='%s' AND is_active=1 AND expires_at>NOW()", eesc, neesc);
                if (mysql_query(db, q) != 0) { skipped++; continue; }
                MYSQL_RES *cres = mysql_store_result(db);
                bool dup = false;
                if (cres && mysql_num_rows(cres) > 0) {
                    MYSQL_ROW crow = mysql_fetch_row(cres);
                    dup = crow && crow[0] && atoll(crow[0]) > 0;
                }
                if (cres) mysql_free_result(cres);
                if (dup) { skipped++; continue; }
                // 3) 插入定向福利
                uint64_t eid = Snowflake::generate();
                snprintf(q, sizeof(q),
                    "INSERT INTO welfare_events (id, name, description, event_type, target_email,"
                    " reward_amount, starts_at, expires_at, is_active)"
                    " VALUES (%llu, '%s', '%s', 'general', '%s', %d,"
                    " NOW(), DATE_ADD(NOW(), INTERVAL %d DAY), 1)",
                    eid, neesc, descsc, eesc, days, expiresDays);
                if (mysql_query(db, q) != 0) { skipped++; continue; }
                granted++;
            }
            mysql_free_result(res);
            return AuthService::jsonOk("已按累计充值≥" + minStr + "元发放福利 " + name
                + " (+" + std::to_string(days) + "天 Pro, " + std::to_string(expiresDays) + "天内可领): 发放 "
                + std::to_string(granted) + " 人, 跳过 " + std::to_string(skipped) + " 人");
        });

    // ── 管理: 退款 (仅限有付费记录用户; 全额退, 标记订单 refunded + 取消订阅) ──
    // 边界: 无 paid 订单 → 不改任何数据 (兑换码/福利订阅不受误伤); 幂等; 返回退款总额
    server.route("POST", "/api/admin/refund",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
            if (tp->email != "admin@solohomie.top")
                return AuthService::jsonErr("FORBIDDEN", "无管理员权限");
            std::string email = StringUtils::jsonVal(body, "email");
            if (email.empty())
                return AuthService::jsonErr("FIELDS_REQUIRED", "email必填");
            bool forceRefund = StringUtils::jsonVal(body, "force") == "true";
            auto u = auth.userStore().findByEmail(email);
            if (!u) return AuthService::jsonErr("NOT_FOUND", "用户不存在");
            auto g = pool.acquire(5000);
            if (!g) return AuthService::jsonErr("DB_ERROR", "db busy");
            MYSQL *db = g->get();
            // 1) 先查 paid 订单 (count + 总额)
            char q[512];
            snprintf(q, sizeof(q),
                "SELECT COUNT(*), COALESCE(SUM(amount),0) FROM payment_orders "
                "WHERE user_id=%llu AND status='paid'", (unsigned long long)u->id);
            if (mysql_query(db, q) != 0)
                return AuthService::jsonErr("DB_ERROR", mysql_error(db));
            MYSQL_RES *res = mysql_store_result(db);
            long long cnt = 0, total = 0;
            if (res && mysql_num_rows(res) > 0) {
                MYSQL_ROW row = mysql_fetch_row(res);
                cnt  = row[0] ? atoll(row[0]) : 0;
                total = row[1] ? atoll(row[1]) : 0;
            }
            if (res) mysql_free_result(res);
            // 2) 无付费记录 → 不动任何数据 (防误伤兑换码/福利订阅)
            if (cnt <= 0)
                return AuthService::jsonOk("该用户无付费记录, 未做任何变更");
            // 2.5) 退款时限: 最新付费 30 分钟内可退 (force=true 人工流程不受限)
            if (!forceRefund) {
                snprintf(q, sizeof(q),
                    "SELECT COUNT(*) FROM payment_orders WHERE user_id=%llu "
                    "AND status='paid' AND created_at >= NOW() - INTERVAL 30 MINUTE",
                    (unsigned long long)u->id);
                if (mysql_query(db, q) != 0)
                    return AuthService::jsonErr("DB_ERROR", mysql_error(db));
                MYSQL_RES *tres = mysql_store_result(db);
                long long recent = 0;
                if (tres && mysql_num_rows(tres) > 0) {
                    MYSQL_ROW trow = mysql_fetch_row(tres);
                    recent = (trow && trow[0]) ? atoll(trow[0]) : 0;
                }
                if (tres) mysql_free_result(tres);
                if (recent <= 0)
                    return AuthService::jsonErr("REFUND_TIMEOUT", "已超过 30 分钟退款时限, 无法退款");
            }
            // 3) 标记订单 refunded + 取消订阅
            // 3) ikunpay refund first (all succeed -> then mark system)
            {
                auto *pay = paymentService();
                if (!pay)
                    return AuthService::jsonErr("PAY_SERVICE_UNAVAILABLE", "PAY_SERVICE_UNAVAILABLE");
                snprintf(q, sizeof(q),
                    "SELECT id, amount FROM payment_orders WHERE user_id=%llu AND status='paid'",
                    (unsigned long long)u->id);
                if (mysql_query(db, q) != 0)
                    return AuthService::jsonErr("DB_ERROR", mysql_error(db));
                MYSQL_RES *ores = mysql_store_result(db);
                if (!ores) return AuthService::jsonErr("DB_ERROR", "no result");
                MYSQL_ROW orow;
                while ((orow = mysql_fetch_row(ores)) != nullptr) {
                    uint64_t oid = strtoull(orow[0], nullptr, 10);
                    long long cents = atoll(orow[1]);
                    char money[32];
                    snprintf(money, sizeof(money), "%.2f", (double)cents / 100.0);
                    if (!forceRefund) {
                        std::string rerr;
                        if (!pay->refundOrder(oid, money, rerr)) {
                            mysql_free_result(ores);
                            return AuthService::jsonErr("REFUND_FAIL", "PLATFORM_REFUND_FAIL: " + rerr);
                        }
                    }
                }
                mysql_free_result(ores);
            }
            // 4) mark orders refunded + clear subscription

            snprintf(q, sizeof(q),
                "UPDATE payment_orders SET status='refunded', refunded_at=NOW() "
                "WHERE user_id=%llu AND status='paid'", (unsigned long long)u->id);
            if (mysql_query(db, q) != 0)
                return AuthService::jsonErr("DB_ERROR", mysql_error(db));
            snprintf(q, sizeof(q),
                "UPDATE users SET subscription_expires=NULL WHERE id=%llu", (unsigned long long)u->id);
            mysql_query(db, q);
            try { RedisClient::instance().del("sub:" + std::to_string(u->id)); }
            catch (const std::exception &e) { std::cerr << "[SubSync] del FAILED sub:" << u->id << ": " << e.what() << std::endl; }
            char buf[160];
            if (forceRefund)
                snprintf(buf, sizeof(buf), "已标记退款 %lld 笔订单共 %lld 分 (人工已退款), 订阅已取消", cnt, total);
            else
                snprintf(buf, sizeof(buf), "已退款 %lld 笔订单共 %lld 分, 订阅已取消", cnt, total);
            std::cout << "[Admin] refund email=" << email << " orders=" << cnt << " total_cents=" << total << (forceRefund ? " force" : "") << std::endl;
            return AuthService::jsonOk(buf);
        });

    // ── 管理: 封禁 ────────────────────────────────
    server.route("POST", "/api/admin/ban",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
            if (tp->email != "admin@solohomie.top")
                return AuthService::jsonErr("FORBIDDEN", "无管理员权限");
            std::string email = StringUtils::jsonVal(body, "email");
            if (email.empty()) return AuthService::jsonErr("FIELDS_REQUIRED", "email必填");
            auto u = auth.userStore().findByEmail(email);
            if (!u) return AuthService::jsonErr("NOT_FOUND", "用户不存在");
            auto g = pool.acquire(5000);
            if (!g) return AuthService::jsonErr("DB_ERROR", "db busy");
            MYSQL *db = g->get();
            char q[512]; char eesc[256];
            mysql_real_escape_string(db, eesc, email.c_str(), email.size());
            snprintf(q, sizeof(q), "UPDATE users SET status=1 WHERE email='%s'", eesc);
            if (mysql_query(db, q) != 0)
                return AuthService::jsonErr("DB_ERROR", mysql_error(db));
            // 在线立即踢出 (5分钟窗口, 客户端5s心跳必能收到)
            try {
                RedisClient::instance().setex("kicked:" + std::to_string(u->id), 300, "1");
            } catch (...) {}
            return AuthService::jsonOk("已封禁 " + email);
        });

    // ── 管理: 解封 ────────────────────────────────
    // ── 兑换码管理: 生成 (2026-08-16) ──
    server.route("POST", "/api/admin/redeem/generate",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
            if (tp->email != "admin@solohomie.top")
                return AuthService::jsonErr("FORBIDDEN", "无管理员权限");
            auto getNum = [&](const std::string &key, uint64_t def) -> uint64_t {
                std::string v = StringUtils::jsonVal(body, key);
                if (v.empty()) return def;
                try { return std::stoull(v); } catch (...) { return def; }
            };
            uint64_t days    = getNum("days", 0);
            uint64_t count   = getNum("count", 1);
            uint64_t maxUses = getNum("max_uses", 1);    // 0=不限次数
            uint64_t expDays = getNum("expires_days", 0); // 0=永久有效
            std::string prefix = StringUtils::jsonVal(body, "prefix");
            if (prefix.empty()) prefix = "BH";
            for (char &c : prefix) c = (char)std::toupper((unsigned char)c);
            if (days < 1 || days > 3650)
                return AuthService::jsonErr("FIELDS_REQUIRED", "days 需在 1-3650 天");
            if (count < 1 || count > 500)
                return AuthService::jsonErr("FIELDS_REQUIRED", "count 需在 1-500");
            if (maxUses > 10000)
                return AuthService::jsonErr("FIELDS_REQUIRED", "max_uses 需为 0(不限) 或 1-10000");
            if (expDays > 3650)
                return AuthService::jsonErr("FIELDS_REQUIRED", "expires_days 需在 0-3650");
            if (prefix.size() > 6)
                return AuthService::jsonErr("FIELDS_REQUIRED", "prefix 需为 1-6 位字母数字");
            for (char c : prefix)
                if (!std::isalnum((unsigned char)c))
                    return AuthService::jsonErr("FIELDS_REQUIRED", "prefix 只能包含字母数字");

            auto g = pool.acquire(5000);
            if (!g) return AuthService::jsonErr("DB_ERROR", "db busy");
            MYSQL *db = g->get();

            static const std::string ALPHA = "ABCDEFGHJKMNPQRSTUVWXYZ23456789"; // 去掉 0O1IL 易混字符
            static std::mt19937_64 rng(
                std::random_device{}() ^ (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count());

            std::vector<std::string> codes;
            char q[1024]; char eesc[64];
            for (uint64_t i = 0; i < count; i++) {
                bool inserted = false;
                for (int attempt = 0; attempt < 3 && !inserted; attempt++) {
                    std::string code = prefix + "-";
                    for (int k = 0; k < 12; k++)
                        code += ALPHA[rng() % ALPHA.size()];
                    uint64_t id = Snowflake::generate();
                    std::string expPart = expDays > 0
                        ? "DATE_ADD(NOW(), INTERVAL " + std::to_string(expDays) + " DAY)" : "NULL";
                    mysql_real_escape_string(db, eesc, code.c_str(), code.size());
                    snprintf(q, sizeof(q),
                        "INSERT INTO redeem_codes (id, code, days, max_uses, used_count, is_active, expires_at) "
                        "VALUES (%llu,'%s',%llu,%llu,0,1,%s)",
                        id, eesc, days, maxUses, expPart.c_str());
                    if (mysql_query(db, q) == 0) { inserted = true; codes.push_back(code); }
                }
                if (!inserted) {
                    return AuthService::jsonErr("GEN_FAILED",
                        "生成失败: 随机码碰撞, 请重试 (已生成 " + std::to_string(codes.size()) + " 个)");
                }
            }

            std::string json = "{\"success\":true,\"message\":\"已生成 " + std::to_string(codes.size())
                             + " 个兑换码\",\"codes\":[";
            for (size_t i = 0; i < codes.size(); i++) {
                if (i) json += ",";
                json += "\"" + AuthService::jsonEscape(codes[i]) + "\"";
            }
            json += "]}";
            return json;
        });

    // ── 兑换码管理: 列表 (每页50条, created_at 倒序) ──
    server.route("GET", "/api/admin/redeem/list",
        [&](const std::string &, const std::string &query, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
            if (tp->email != "admin@solohomie.top")
                return AuthService::jsonErr("FORBIDDEN", "无管理员权限");
            std::string pageStr = StringUtils::queryVal(query, "page");
            uint64_t page = 1;
            if (!pageStr.empty()) { try { page = std::stoull(pageStr); } catch (...) { page = 1; } }
            if (page < 1) page = 1;
            uint64_t offset = (page - 1) * 50;

            auto g = pool.acquire(5000);
            if (!g) return AuthService::jsonErr("DB_ERROR", "db busy");
            MYSQL *db = g->get();
            char q[512];
            snprintf(q, sizeof(q),
                "SELECT id, code, days, max_uses, used_count, is_active, expires_at, created_at "
                "FROM redeem_codes ORDER BY created_at DESC, id DESC LIMIT 50 OFFSET %llu", offset);
            mysql_query(db, q);
            MYSQL_RES *res = mysql_store_result(db);
            std::string json = "{\"success\":true,\"codes\":[";
            bool first = true;
            MYSQL_ROW row;
            while (res && (row = mysql_fetch_row(res))) {
                if (!first) json += ","; first = false;
                json += "{\"code\":\"" + AuthService::jsonEscape(row[1] ? row[1] : "")
                     + "\",\"days\":" + std::string(row[2] ? row[2] : "0")
                     + ",\"max_uses\":" + std::string(row[3] ? row[3] : "0")
                     + ",\"used_count\":" + std::string(row[4] ? row[4] : "0")
                     + ",\"is_active\":" + std::string(row[5] ? row[5] : "0")
                     + ",\"expires_at\":\"" + AuthService::jsonEscape(row[6] ? row[6] : "") + "\""
                     + ",\"created_at\":\"" + AuthService::jsonEscape(row[7] ? row[7] : "") + "\"}";
            }
            json += "]";
            if (res) mysql_free_result(res);
            // 汇总: 总数 / 生效数 / 总使用次数
            snprintf(q, sizeof(q),
                "SELECT COUNT(*), COALESCE(SUM(is_active=1),0), COALESCE(SUM(used_count),0) FROM redeem_codes");
            mysql_query(db, q);
            MYSQL_RES *res2 = mysql_store_result(db);
            if (res2) {
                MYSQL_ROW r2 = mysql_fetch_row(res2);
                if (r2) {
                    json += ",\"total\":" + std::string(r2[0] ? r2[0] : "0")
                         +  ",\"active\":" + std::string(r2[1] ? r2[1] : "0")
                         +  ",\"total_used\":" + std::string(r2[2] ? r2[2] : "0");
                }
                mysql_free_result(res2);
            }
            json += "}";
            return json;
        });

    // ── 兑换码管理: 停用 (不影响已兑换用户) ──
    server.route("POST", "/api/admin/redeem/disable",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
            if (tp->email != "admin@solohomie.top")
                return AuthService::jsonErr("FORBIDDEN", "无管理员权限");
            std::string code = StringUtils::jsonVal(body, "code");
            size_t bs = code.find_first_not_of(" \t"), be = code.find_last_not_of(" \t");
            code = (bs == std::string::npos) ? "" : code.substr(bs, be - bs + 1);
            for (char &c : code) c = (char)std::toupper((unsigned char)c);
            if (code.empty())
                return AuthService::jsonErr("FIELDS_REQUIRED", "code 必填");
            auto g = pool.acquire(5000);
            if (!g) return AuthService::jsonErr("DB_ERROR", "db busy");
            MYSQL *db = g->get();
            char q[512]; char eesc[64];
            mysql_real_escape_string(db, eesc, code.c_str(), code.size());
            snprintf(q, sizeof(q), "UPDATE redeem_codes SET is_active=0 WHERE code='%s'", eesc);
            if (mysql_query(db, q) != 0)
                return AuthService::jsonErr("DB_ERROR", mysql_error(db));
            if (mysql_affected_rows(db) == 0)
                return AuthService::jsonErr("NOT_FOUND", "兑换码不存在: " + code);
            return AuthService::jsonOk("已停用兑换码: " + code);
        });

    server.route("POST", "/api/admin/unban",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
            if (tp->email != "admin@solohomie.top")
                return AuthService::jsonErr("FORBIDDEN", "无管理员权限");
            std::string email = StringUtils::jsonVal(body, "email");
            if (email.empty()) return AuthService::jsonErr("FIELDS_REQUIRED", "email必填");
            auto u = auth.userStore().findByEmail(email);
            if (!u) return AuthService::jsonErr("NOT_FOUND", "用户不存在");
            auto g = pool.acquire(5000);
            if (!g) return AuthService::jsonErr("DB_ERROR", "db busy");
            MYSQL *db = g->get();
            char q[512]; char eesc[256];
            mysql_real_escape_string(db, eesc, email.c_str(), email.size());
            snprintf(q, sizeof(q), "UPDATE users SET status=0 WHERE email='%s'", eesc);
            if (mysql_query(db, q) != 0)
                return AuthService::jsonErr("DB_ERROR", mysql_error(db));
            // 清理可能残留的踢出标记 (幂等, 保证解封后立即可登录)
            try {
                RedisClient::instance().del("kicked:" + std::to_string(u->id));
            } catch (...) {}
            return AuthService::jsonOk("已解封 " + email);
        });

}
