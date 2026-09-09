#include "UpdateRoutes.h"
#include "../core/server/HttpServer.h"
#include "../core/server/TlsConnection.h"
#include "../common/StringUtils.h"
#include "../core/config/Env.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mswsock.h>
#endif

#include <sstream>
#include <fstream>
#include <vector>
#include <filesystem>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>

// ── inline MD5 (RFC 1321, 仅供 manifest 校验使用) ──
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
} // anonymous namespace

// ── 流式文件发送: 读整个文件到内存 (避免 TOCTOU 竞态) 再满带宽发送 ──
static void sendFile(socket_t sock, TlsConnection *tls, const std::string &fullPath,
                     const std::string &contentType, const std::string &attachName) {
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
    hdr << "HTTP/1.1 200 OK\r\n"
        << "Content-Type: " << contentType << "\r\n"
        << "Content-Length: " << fsize << "\r\n";
    if (!attachName.empty())
        hdr << "Content-Disposition: attachment; filename=\"" << attachName << "\"\r\n";
    hdr << "Cache-Control: public, max-age=86400\r\n"
        << "Connection: close\r\n"
        << "Access-Control-Allow-Origin: *\r\n\r\n";
    std::string h = hdr.str();
    // 发送 header（检查返回值）
    { const char *p = h.data(); size_t left = h.size(); while (left > 0) { auto sent = tls ? tls->write(p, (int)left) : send(sock, p, (int)left, 0); if (sent <= 0) return; left -= sent; p += sent; } }
    // 发送 body
    { const char *p = fileBuf.data(); size_t left = fsize; while (left > 0) { auto sent = tls ? tls->write(p, (int)left) : send(sock, p, (int)left, 0); if (sent <= 0) break; left -= sent; p += sent; } }
}

void registerUpdateRoutes(HttpServer &server) {

    // ── 系统版本 (旧接口, 2026-09-09 自 main.cpp 迁入; heartbeat 已合并版本检查,
    //    本接口直接返回 version.json 原文) ──
    server.route("GET", "/api/system/version",
        [](const std::string &, const std::string &, const std::string &) {
            std::ifstream f("version.json");
            if (!f) return std::string("{\"latest_version\":\"1.0.0\"}");
            std::ostringstream ss; ss << f.rdbuf();
            return ss.str();
        });

    // ── 版本检查 / 自动更新 ────────────────────────
    server.route("GET", "/api/version",
        [](const std::string &, const std::string &query, const std::string &) {
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
                + ",\"changelog\":\"" + StringUtils::jsonEscape(changelog) + "\""
                + "}";
        });

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
            sendFile(sock, tls, Env::exeDir() + "/releases/BallsHackPro-latest.zip",
                     "application/zip", "BallsHackPro-update.zip");
        });

    // ── 更新文件下载: /api/dl?file=xxx → releases/xxx ──
    // GET 下载本体; HEAD 供探测(同 handler, body 客户端忽略) 2026-08-29
    auto dlHandler = [](socket_t sock, TlsConnection *tls, const std::string &query) {
            std::string file = StringUtils::queryVal(query, "file");
            if (file.empty() || file.find("..") != std::string::npos) {
                std::string err = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                if (tls) tls->write(err.data(), (int)err.size());
                else send(sock, err.data(), (int)err.size(), 0);
                return;
            }
            sendFile(sock, tls, Env::exeDir() + "/releases/" + file,
                     "application/octet-stream", "");
    };
    server.routeStream("GET", "/api/dl", dlHandler);
    server.routeStream("HEAD", "/api/dl", dlHandler);

    // ── 下载安装包 (满带宽发送) ──
    server.routeStream("GET", "/api/download",
        [](socket_t sock, TlsConnection *tls, const std::string &) {
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
            sendFile(sock, tls, Env::exeDir() + "/releases/BallsHackPro-setup-latest.exe",
                     "application/octet-stream", filename);
        });
}
