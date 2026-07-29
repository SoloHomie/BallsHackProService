#include "AuthRoutes.h"

#include "../core/server/HttpServer.h"
#include "../auth/AuthService.h"
#include "../auth/SessionManager.h"
#include "../auth/TokenService.h"
#include "../common/StringUtils.h"
#include "../view/OAuthPage.h"
#include "../core/cache/RedisClient.h"
#include "../core/database/DbPool.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

// ── OAuth URL 构建辅助（github / discord 共用）────────────
static std::string buildOAuthUrl(AuthService &auth,
                                  const std::string &provider,
                                  const std::string &query) {
    std::string state;
    std::string url = auth.oauthGetUrl(provider, state);
    if (url.empty()) return "";

    std::string portStr = StringUtils::queryVal(query, "cbport");
    std::string bindUid = StringUtils::queryVal(query, "bind_uid");
    std::string oldState = state;
    if (!portStr.empty()) state += ":" + portStr;
    std::string cleanState = state;
    if (!bindUid.empty()) state += ":bind:" + bindUid;

    if (state != oldState) {
        try {
            auto val = RedisClient::instance().get("oauth:state:" + oldState);
            if (val) {
                RedisClient::instance().setex("oauth:state:" + state, 600, *val);
                if (!bindUid.empty())
                    RedisClient::instance().setex("oauth:state:" + cleanState, 600, *val);
                RedisClient::instance().del("oauth:state:" + oldState);
            }
        } catch (...) {}
        url = StringUtils::replace(url, oldState, state);
    }
    return url;
}

void registerAuthRoutes(HttpServer &server, AuthService &auth, DbPool &pool) {

    // ── 发送验证码 ──────────────────────────────────
    server.route("POST", "/api/auth/send-code",
        [&](const std::string &body, const std::string &, const std::string &) {
            std::string purpose = StringUtils::jsonVal(body, "purpose");
            if (purpose.empty()) purpose = "register";
            return auth.sendCode(StringUtils::jsonVal(body, "email"), purpose);
        });

    // ── 密码登录 ──────────────────────────────────
    server.route("POST", "/api/auth/login",
        [&](const std::string &body, const std::string &, const std::string &) {
            return auth.login(StringUtils::jsonVal(body, "email"),
                              StringUtils::jsonVal(body, "password"));
        });

    // ── 验证码登录 ────────────────────────────────
    server.route("POST", "/api/auth/verify-login",
        [&](const std::string &body, const std::string &, const std::string &) {
            return auth.loginWithCode(StringUtils::jsonVal(body, "email"),
                                      StringUtils::jsonVal(body, "code"));
        });

    // ── 注册 ──────────────────────────────────────
    server.route("POST", "/api/auth/register",
        [&](const std::string &body, const std::string &, const std::string &) {
            auto result = auth.doRegister(StringUtils::jsonVal(body, "email"),
                                          StringUtils::jsonVal(body, "password"),
                                          StringUtils::jsonVal(body, "code"));
            if (result.find("\"success\":true") != std::string::npos) {
                auto user = auth.userStore().findByEmail(
                    StringUtils::jsonVal(body, "email"));
            }
            return result;
        });

    // ── 重置密码 ──────────────────────────────────
    server.route("POST", "/api/auth/reset-password",
        [&](const std::string &body, const std::string &, const std::string &) {
            return auth.resetPassword(StringUtils::jsonVal(body, "email"),
                                      StringUtils::jsonVal(body, "code"),
                                      StringUtils::jsonVal(body, "newPassword"));
        });

    // ── GitHub OAuth URL ─────────────────────────
    server.route("GET", "/api/auth/oauth/github",
        [&](const std::string &, const std::string &query, const std::string &) {
            std::string url = buildOAuthUrl(auth, "github", query);
            if (url.empty()) return AuthService::jsonErr("UNSUPPORTED", "不支持的登录方式");
            return "{\"success\":true,\"oauth_url\":\"" + AuthService::jsonEscape(url) + "\"}";
        });

    // ── Discord OAuth URL ────────────────────────
    server.route("GET", "/api/auth/oauth/discord",
        [&](const std::string &, const std::string &query, const std::string &) {
            std::string url = buildOAuthUrl(auth, "discord", query);
            if (url.empty()) return AuthService::jsonErr("UNSUPPORTED", "不支持的登录方式");
            return "{\"success\":true,\"oauth_url\":\"" + AuthService::jsonEscape(url) + "\"}";
        });

    // ── OAuth 回调（浏览器重定向目标）─────────────
    server.route("GET", "/api/auth/oauth/callback",
        [&](const std::string &, const std::string &query, const std::string &) -> std::string {
            std::string code  = StringUtils::queryVal(query, "code");
            std::string state = StringUtils::queryVal(query, "state");
            if (code.empty() || state.empty()) {
                OAuthPage::OAuthCallbackData empty;
                empty.provider = "unknown";
                return OAuthPage::render(empty);
            }

            auto sep1 = state.find(':');
            std::string provider = (sep1 != std::string::npos)
                ? state.substr(0, sep1) : state;

            std::string cbport, bindUid;
            auto bindPos = state.find(":bind:");
            std::string cleanState = (bindPos != std::string::npos)
                ? state.substr(0, bindPos) : state;
            if (bindPos != std::string::npos)
                bindUid = state.substr(bindPos + 6);
            auto lastColon = cleanState.rfind(':');
            if (lastColon != std::string::npos && lastColon > sep1)
                cbport = cleanState.substr(lastColon + 1);

            std::string result;
            if (!bindUid.empty()) {
                result = auth.oauthCallbackBind(provider, code, cleanState,
                                                 std::stoull(bindUid));
            } else {
                result = auth.oauthCallback(provider, code, cleanState);
            }
            bool success = result.find("\"success\":true") != std::string::npos;

            OAuthPage::OAuthCallbackData data;
            data.success      = success;
            data.provider     = provider;
            data.mode         = bindUid.empty() ? "" : "bind";
            data.email        = StringUtils::jsonVal(result, "email");
            data.username     = StringUtils::jsonVal(result, "username");
            data.avatar       = StringUtils::jsonVal(result, "avatar");
            data.token        = StringUtils::jsonVal(result, "access_token");
            data.refreshToken = StringUtils::jsonVal(result, "refresh_token");
            data.uid          = StringUtils::jsonVal(result, "uid");

            if (success) {
                auto oauthUser = auth.userStore().findByEmail(data.email);
            }

            return OAuthPage::render(data, "html/OAuthCallback.html", cbport);
        });

    // ── OAuth 绑定列表 ────────────────────────────
    server.route("GET", "/api/auth/oauth/links",
        [&](const std::string &, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
            auto links = auth.userStore().listOAuthLinks(tp->email);
            std::ostringstream json;
            json << "{\"success\":true,\"links\":[";
            for (size_t i = 0; i < links.size(); ++i) {
                if (i > 0) json << ",";
                json << "{\"provider\":\"" << std::get<0>(links[i])
                     << "\",\"provider_id\":\"" << std::get<1>(links[i])
                     << "\",\"provider_uid\":\"" << std::get<2>(links[i])
                     << "\"}";
            }
            json << "]}";
            return json.str();
        });

    // ── OAuth 解绑 ────────────────────────────────
    server.route("POST", "/api/auth/oauth/unlink",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
            std::string provider = StringUtils::jsonVal(body, "provider");
            if (provider.empty())
                return AuthService::jsonErr("FIELDS_REQUIRED", "请指定要解绑的平台");
            if (auth.userStore().unlinkOAuth(tp->email, provider))
                return AuthService::jsonOk("解绑成功");
            return AuthService::jsonErr("UNLINK_FAILED", "解绑失败，请稍后重试");
        });

    // ── Token 刷新 ────────────────────────────────
    server.route("POST", "/api/auth/refresh",
        [&](const std::string &body, const std::string &, const std::string &) {
            std::string refreshToken = StringUtils::jsonVal(body, "refresh_token");
            if (refreshToken.empty())
                return AuthService::jsonErr("TOKEN_REQUIRED", "请提供 refresh_token");

            auto data = SessionManager::getRefresh(refreshToken, &pool);
            if (!data)
                return AuthService::jsonErr("INVALID_TOKEN", "登录凭证无效或已过期，请重新登录");

            std::string sub = StringUtils::jsonVal(*data, "sub");
            std::string email = StringUtils::jsonVal(*data, "email");
            std::string provider = StringUtils::jsonVal(*data, "provider");

            if (sub.empty())
                return AuthService::jsonErr("INVALID_TOKEN", "会话数据损坏，请重新登录");

            SessionManager::revokeRefresh(refreshToken);

            TokenPayload tp;
            tp.sub      = sub;
            tp.email    = email;
            tp.provider = provider;
            tp.jti      = SessionManager::generateJti();
            std::string newAccess = auth.tokenService().generateAccess(tp, 900);

            std::string newRefresh = TokenService::generateRefresh();
            SessionManager::storeRefresh(newRefresh, sub, email, provider, 30*24*3600, &pool);

            std::ostringstream resp;
            resp << "{\"success\":true"
                 << ",\"access_token\":\"" << AuthService::jsonEscape(newAccess) << "\""
                 << ",\"refresh_token\":\"" << AuthService::jsonEscape(newRefresh) << "\""
                 << ",\"token_type\":\"Bearer\""
                 << ",\"expires_in\":900"
                 << ",\"uid\":\"" << AuthService::jsonEscape(sub) << "\""
                 << ",\"email\":\"" << AuthService::jsonEscape(email) << "\"";
            auto refUser = auth.userStore().findByEmail(email);
            if (refUser) {
                resp << ",\"username\":\"" << AuthService::jsonEscape(refUser->username) << "\"";
            }
            resp << ",\"provider\":\"" << AuthService::jsonEscape(provider) << "\"";
            resp << "}";
            return resp.str();
        });

    // ── 登出 ──────────────────────────────────────
    server.route("POST", "/api/auth/logout",
        [&](const std::string &body, const std::string &, const std::string &) {
            std::string refreshToken = StringUtils::jsonVal(body, "refresh_token");
            if (!refreshToken.empty())
                SessionManager::revokeRefresh(refreshToken);
            return AuthService::jsonOk("已退出登录");
        });

    // ── 心跳 / 版本检查 ──────────────────────────
    server.route("POST", "/api/auth/heartbeat",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            std::string clientVer = StringUtils::jsonVal(body, "client_version");

            // 读 version.json
            std::string latestVer = "1.0.0", downloadUrl = "", releaseNotes = "";
            std::ifstream fv("version.json");
            if (fv) {
                std::ostringstream ss; ss << fv.rdbuf();
                std::string json = ss.str();
                latestVer   = StringUtils::jsonVal(json, "latest_version");
                downloadUrl = StringUtils::jsonVal(json, "download_url");
                releaseNotes = StringUtils::jsonVal(json, "release_notes");
                if (latestVer.empty()) latestVer = "1.0.0";
            }

            bool updateAvailable = (!clientVer.empty() && clientVer != latestVer);

            // 订阅状态（需登录）—— Redis 缓存 + MySQL 兜底
            bool subscribed = false;
            std::string subExpires = "";
            auto tp = auth.authenticate(headers);
            if (tp) {
                // 1. 先查 Redis 缓存
                bool cached = false;
                try {
                    auto &r = RedisClient::instance();
                    auto val = r.get("sub:" + tp->sub);
                    if (val) {
                        cached = true;
                        subExpires = *val;
                    }
                } catch (...) {}
                if (cached) {
                    auto now = std::chrono::system_clock::now();
                    auto nowT = std::chrono::system_clock::to_time_t(now);
                    struct tm expireTm = {};
                    std::istringstream ss(subExpires);
                    ss >> std::get_time(&expireTm, "%Y-%m-%d %H:%M:%S");
                    auto expireT = std::mktime(&expireTm);
                    subscribed = (expireT > nowT);
                }
                // 2. 缓存未命中 → 查 MySQL 并回写 Redis
                if (!cached) {
                    auto user = auth.userStore().findByEmail(tp->email);
                    if (user && !user->subscription_expires.empty()) {
                        subExpires = user->subscription_expires;
                        auto now = std::chrono::system_clock::now();
                        auto nowT = std::chrono::system_clock::to_time_t(now);
                        struct tm expireTm = {};
                        std::istringstream ss(subExpires);
                        ss >> std::get_time(&expireTm, "%Y-%m-%d %H:%M:%S");
                        auto expireT = std::mktime(&expireTm);
                        subscribed = (expireT > nowT);
                        // 回写 Redis，TTL = 距过期秒数
                        try {
                            int d = (int)(expireT - nowT);
                            int ttl = subscribed ? (d > 60 ? d : 60) : 60;
                            RedisClient::instance().setex("sub:" + tp->sub, ttl, subExpires);
                        } catch (...) {}
                    }
                }
            }

            std::ostringstream resp;
            resp << "{"
                 << "\"success\":true"
                 << ",\"latest_version\":\"" << AuthService::jsonEscape(latestVer) << "\""
                 << ",\"update_available\":" << (updateAvailable ? "true" : "false")
                 << ",\"download_url\":\"" << AuthService::jsonEscape(downloadUrl) << "\""
                 << ",\"release_notes\":\"" << AuthService::jsonEscape(releaseNotes) << "\""
                 << ",\"subscribed\":" << (subscribed ? "true" : "false")
                 << ",\"subscription_expires\":\"" << AuthService::jsonEscape(subExpires) << "\""
                 << "}";
            return resp.str();
        });
    // username update
    server.route("POST", "/api/user/update-profile",
        [&](const std::string &body, const std::string &, const std::string &h) {
            auto tp = auth.authenticate(h);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "login required");
            std::string nn = StringUtils::jsonVal(body, "username");
            if (nn.empty()) return AuthService::jsonErr("FIELDS_REQUIRED", "username required");
            if (nn.size() < 2 || nn.size() > 20) return AuthService::jsonErr("INVALID_NAME", "2-20 chars");
            // 过滤特殊字符, 防SQL注入
            for (char c : nn) { if (!isalnum((unsigned char)c) && c != '_' && c != '-') return AuthService::jsonErr("INVALID_NAME", "only a-z 0-9 _ -"); }
            auto g = pool.acquire(5000);
            if (!g) return AuthService::jsonErr("DB_ERROR", "db busy");
            // 参数化查询
            MYSQL_STMT *stmt = mysql_stmt_init(g->get());
            const char* q = "UPDATE users SET username=? WHERE id=?";
            mysql_stmt_prepare(stmt, q, strlen(q));
            MYSQL_BIND b[2] = {};
            b[0].buffer_type = MYSQL_TYPE_STRING; b[0].buffer = (void*)nn.c_str(); b[0].buffer_length = nn.size();
            unsigned long long uid = std::stoull(tp->sub);
            b[1].buffer_type = MYSQL_TYPE_LONGLONG; b[1].buffer = &uid;
            mysql_stmt_bind_param(stmt, b);
            mysql_stmt_execute(stmt);
            mysql_stmt_close(stmt);
            return AuthService::jsonOk("ok");
        });
}
