#include "SystemRoutes.h"

#include "../core/server/HttpServer.h"
#include "../core/server/TlsConnection.h"
#include "../auth/AuthService.h"
#include "../common/StringUtils.h"
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

            std::string directDl = "http://38.92.9.178:8920/api/download";
            std::string cloudDl = "https://api.solohomie.top/api/download";

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
                + ",\"download_url\":\"" + cloudDl + "\""
                + ",\"direct_download_url\":\"" + directDl + "\""
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
            try { auto u = auth.userStore().findByEmail(email);
                if (u) RedisClient::instance().del("sub:" + std::to_string(u->id)); } catch(...) {}
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
                 << ",\"subscription_expires\":\"" << AuthService::jsonEscape(user->subscription_expires) << "\"";
            // 计算剩余天数
            if (!user->subscription_expires.empty()) {
                struct tm et = {}; std::istringstream ss(user->subscription_expires);
                ss >> std::get_time(&et, "%Y-%m-%d %H:%M:%S");
                auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                int rem = (int)(std::mktime(&et) - now) / 86400;
                resp << ",\"remaining_days\":" << rem;
            } else { resp << ",\"remaining_days\":0"; }
            resp << "}";
            return resp.str();
        });

    // ── 管理: 在线用户列表 ─────────────────────────
    // ── 更新文件下载: /api/dl?file=xxx → releases/xxx ──
    server.routeStream("GET", "/api/dl",
        [](socket_t sock, TlsConnection *tls, const std::string &query) {
            std::string file = StringUtils::queryVal(query, "file");
            if (file.empty() || file.find("..") != std::string::npos) {
                std::string err = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                if (tls) tls->write(err.data(), (int)err.size());
                else send(sock, err.data(), (int)err.size(), 0);
                return;
            }
            std::string fullPath = Env::exeDir() + "/releases/" + file;
            std::ifstream fst(fullPath, std::ios::binary | std::ios::ate);
            if (!fst) {
                std::string err = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                if (tls) tls->write(err.data(), (int)err.size());
                else send(sock, err.data(), (int)err.size(), 0);
                return;
            }
            size_t fsize = fst.tellg(); fst.close();
            std::ostringstream hdr;
            hdr << "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                << "Content-Length: " << fsize << "\r\nConnection: close\r\n\r\n";
            std::string h = hdr.str();
            if (tls) tls->write(h.data(), (int)h.size());
            else send(sock, h.data(), (int)h.size(), 0);
            std::ifstream fs(fullPath, std::ios::binary);
            char buf[65536];
            while (fs.read(buf, sizeof(buf)) || fs.gcount() > 0) {
                size_t n = fs.gcount();
                if (tls) tls->write(buf, (int)n);
                else send(sock, buf, (int)n, 0);
            }
        });

    // ── 增量更新文件列表 ─────────────────────────
    server.route("GET", "/api/update/manifest",
        [](const std::string &, const std::string &, const std::string &) {
            std::string dir = Env::exeDir() + "/releases/latest";
            std::ostringstream resp; resp << "[";
#ifndef _WIN32
            std::string cmd = "find \"" + dir + "\" -type f 2>/dev/null";
            FILE *fp = popen(cmd.c_str(), "r");
            if (fp) {
                char buf[1024]; bool first = true;
                while (fgets(buf, sizeof(buf), fp)) {
                    std::string path(buf); path.pop_back();
                    std::string rel = path.substr(dir.size() + 1);
                    std::string md5cmd = "md5sum \"" + path + "\" | awk '{print $1}'";
                    FILE *mf = popen(md5cmd.c_str(), "r");
                    std::string md5; if (mf) { char m[64]={}; fgets(m,64,mf); pclose(mf); md5=m; md5.pop_back(); }
                    struct stat st; stat(path.c_str(), &st);
                    if (!first) resp << ","; first = false;
                    resp << "{\"path\":\"" << rel << "\",\"md5\":\"" << md5
                         << "\",\"size\":" << st.st_size
                         << ",\"url\":\"/api/dl?file=latest/" << rel << "\"}";
                }
                pclose(fp);
            }
#endif
            resp << "]";
            return resp.str();
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
                    r.setex("kicked:" + uid, 60, "1");
                } else {
                    // 通过 email 找 uid
                    auto user = auth.userStore().findByEmail(email);
                    if (user) r.setex("kicked:" + std::to_string(user->id), 60, "1");
                }
            } catch (...) {}
            return AuthService::jsonOk("已踢出，用户下次心跳将被强制下线");
        });

    // ── 下载更新包 (零拷贝 + 大缓冲, 满带宽发送) ──
    server.routeStream("GET", "/api/download",
        [](socket_t sock, TlsConnection *tls, const std::string &) {
            std::string path = Env::exeDir() + "/releases/BallsHackPro-latest.zip";

            // 获取文件大小
            std::ifstream fs(path, std::ios::binary | std::ios::ate);
            if (!fs) {
                std::string err = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                if (tls) tls->write(err.data(), (int)err.size());
                else send(sock, err.data(), (int)err.size(), 0);
                return;
            }
            size_t fsize = fs.tellg();
            fs.close();

            // 发送 HTTP 头
            {
                std::ostringstream hdr;
                hdr << "HTTP/1.1 200 OK\r\n"
                    << "Content-Type: application/octet-stream\r\n"
                    << "Content-Length: " << fsize << "\r\n"
                    << "Content-Disposition: attachment; filename=\"BallsHackPro-latest.zip\"\r\n"
                    << "Cache-Control: public, max-age=86400\r\n"
                    << "Connection: close\r\n"
                    << "Access-Control-Allow-Origin: *\r\n\r\n";
                std::string h = hdr.str();
                if (tls) tls->write(h.data(), (int)h.size());
                else send(sock, h.data(), (int)h.size(), 0);
            }
            // 分块流式发送
            std::ifstream f(path, std::ios::binary);
            constexpr size_t CHUNK = 256 * 1024;
            std::vector<char> chunk(CHUNK);
            while (f.read(chunk.data(), CHUNK) || f.gcount() > 0) {
                size_t n = f.gcount();
                if (tls) tls->write(chunk.data(), (int)n);
#ifdef _WIN32
                else send(sock, chunk.data(), (int)n, 0);
#else
                else send(sock, chunk.data(), n, 0);
#endif
            }
        });
}
