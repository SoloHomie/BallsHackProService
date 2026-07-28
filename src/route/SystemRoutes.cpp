#include "SystemRoutes.h"

#include "../core/server/HttpServer.h"
#include "../auth/AuthService.h"
#include "../common/StringUtils.h"
#include "../core/database/DbPool.h"
#include "../core/config/ServerConfig.h"
#include "../core/config/Env.h"

#include <sstream>
#include <fstream>
#include <vector>
#include <ctime>

void registerSystemRoutes(HttpServer &server, AuthService &auth, DbPool &pool) {

    // ── 版本检查 / 自动更新 ────────────────────────
    server.route("GET", "/api/version",
        [&](const std::string &, const std::string &query, const std::string &) {
            std::string clientVer = StringUtils::queryVal(query, "current");
            std::string latestVer = "1.0.0", minVer = "1.0.0", changelog = "";

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
                if (!lv.empty()) latestVer = lv;
                if (!mv.empty()) minVer = mv;
                if (!cl.empty()) changelog = cl;
            }

            bool force = false;
            if (!clientVer.empty() && !minVer.empty()) {
                auto splitVer = [](const std::string &v) -> std::vector<int> {
                    std::vector<int> parts;
                    std::stringstream ss(v);
                    std::string part;
                    while (std::getline(ss, part, '.')) {
                        try { parts.push_back(std::stoi(part)); }
                        catch (...) { parts.push_back(0); }
                    }
                    return parts;
                };
                auto cv = splitVer(clientVer), mv = splitVer(minVer);
                size_t n = (std::min)(cv.size(), mv.size());
                for (size_t i = 0; i < n; ++i) {
                    if (cv[i] < mv[i]) { force = true; break; }
                    if (cv[i] > mv[i]) break;
                }
            }
            return std::string("{\"success\":true")
                + ",\"latest_version\":\"" + latestVer + "\""
                + ",\"min_version\":\"" + minVer + "\""
                + ",\"force_update\":" + (force ? "true" : "false")
                + ",\"update_type\":\"" + (force ? "force" : "recommended") + "\""
                + ",\"github_url\":\"" + ServerConfig::DOWNLOAD_URL + "\""
                + ",\"download_url\":\"https://api.solohomie.top/api/download\""
                + ",\"changelog\":\"" + changelog + "\""
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

    // ── 健康检查 ──────────────────────────────────
    server.route("GET", "/api/health",
        [&](const std::string &, const std::string &, const std::string &) {
            return "{\"success\":true,\"message\":\"ok\",\"users\":" +
                   std::to_string(auth.userStore().count()) + "}";
        });

    // ── 下载更新包 ────────────────────────────────
    server.route("GET", "/api/download",
        [](const std::string &, const std::string &, const std::string &) -> std::string {
            std::string path = Env::exeDir() + "/releases/Surety-latest.zip";
            std::ifstream f(path, std::ios::binary | std::ios::ate);
            if (!f) return "{\"success\":false,\"message\":\"not found\"}";
            std::streamsize size = f.tellg();
            f.seekg(0);
            std::string data(size, '\0');
            f.read(&data[0], size);
            return "BIN:" + data;
        });
}
