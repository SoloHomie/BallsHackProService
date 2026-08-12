#include "AuthService.h"
#include "CodeManager.h"
#include "UserStore.h"
#include "TokenService.h"
#include "SessionManager.h"
#include "../core/database/DbPool.h"
#include "../common/Snowflake.h"
#include "../email/EmailService.h"
#include "../auth/OAuthService.h"
#include "../core/config/ServerConfig.h"
#include <thread>
#include "../core/config/Env.h"
#include <mysql/mysql.h>
#include <string>

static std::string JWT_SECRET = Env::required("BH_JWT_SECRET");

AuthService::AuthService(DbPool& pool)
    : m_pool(pool)
    , m_users(pool)
    , m_email(std::make_unique<EmailService>())
    , m_oauth(std::make_unique<OAuthService>())
    , m_token(JWT_SECRET) {}
AuthService::~AuthService() = default;

// ── JSON utilities ──────────────────────────────────
std::string AuthService::jsonEscape(const std::string &s) {
    std::string r;
    r.reserve(s.size() + 4);
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
}

std::string AuthService::jsonOk(const std::string &msg) {
    return "{\"success\":true,\"message\":\"" + jsonEscape(msg) + "\"}";
}
std::string AuthService::jsonOk(const std::string &msg,
                                const std::string &k, const std::string &v) {
    return "{\"success\":true,\"message\":\"" + jsonEscape(msg) +
           "\",\"" + jsonEscape(k) + "\":\"" + jsonEscape(v) + "\"}";
}
std::string AuthService::jsonErr(const std::string &code, const std::string &msg) {
    return "{\"success\":false,\"errcode\":\"" + jsonEscape(code) +
           "\",\"message\":\"" + jsonEscape(msg) + "\"}";
}

// ── sendCode ─────────────────────────────────────────
std::string AuthService::sendCode(const std::string &email,
                                   const std::string &purpose) {
    if (email.empty())
        return jsonErr("EMAIL_REQUIRED", "请输入邮箱地址");
    if (!validEmail(email))
        return jsonErr("INVALID_EMAIL", "邮箱格式不正确");

    if (purpose != CodePurpose::REGISTER &&
        purpose != CodePurpose::LOGIN &&
        purpose != CodePurpose::RESET &&
        purpose != CodePurpose::BOT_VERIFY)
        return jsonErr("INVALID_PURPOSE", "操作类型无效");

    bool exists = m_users.exists(email);
    if (purpose == CodePurpose::REGISTER && exists)
        return jsonErr("EMAIL_EXISTS", "该邮箱已被注册，请直接登录");
    if ((purpose == CodePurpose::LOGIN ||
         purpose == CodePurpose::RESET ||
         purpose == CodePurpose::BOT_VERIFY) && !exists)
        return jsonErr("ACCOUNT_NOT_FOUND", "该账号不存在，请先注册");

    int cd = m_codes.cooldownRemaining(email, purpose);
    if (cd > 0)
        return jsonErr("COOLDOWN", "验证码发送过于频繁，请 " + std::to_string(cd) + " 秒后再试");

    std::string code = m_codes.generate(email, purpose);
    if (code.empty())
        return jsonErr("SEND_FAILED", "验证码生成失败，请稍后重试");

    // 开发模式：跳过 SMTP，直接返回验证码
    if (!Env::get("BH_SMTP_MOCK").empty()) {
        return "{\"success\":true,\"message\":\"验证码: " + code + "\",\"code\":\"" + code + "\"}";
    }

    // SMTP 异步发送 — 国内用户到海外邮件服务器延迟高,
    // 同步等待会导致客户端超时断开, 改为后台发送立即返回
    std::string codeCopy = code;
    std::string emailCopy = email;
    std::string purposeCopy = purpose;
    int ttlCopy = ServerConfig::CODE_TTL_SEC;
    // EmailService::sendVerifyCode 每次新建 CURL handle, 线程安全
    EmailService *emailPtr = m_email.get();
    std::thread([emailPtr, emailCopy, codeCopy, ttlCopy, purposeCopy]() {
        emailPtr->sendVerifyCode(emailCopy, codeCopy, ttlCopy, purposeCopy);
    }).detach();

    return jsonOk("验证码已发送，请查收邮箱");
}

// ── login (password) ────────────────────────────────
std::string AuthService::login(const std::string &email, const std::string &password) {
    if (email.empty() || password.empty())
        return jsonErr("FIELDS_REQUIRED", "请输入邮箱和密码");
    if (!validEmail(email))
        return jsonErr("INVALID_EMAIL", "邮箱格式不正确");
    if (!m_users.exists(email))
        return jsonErr("ACCOUNT_NOT_FOUND", "该账号不存在，请先注册");
    if (!m_users.verifyPassword(email, password))
        return jsonErr("INVALID_CREDENTIALS", "密码错误，请检查后重试");

    auto user = m_users.findByEmail(email);

    TokenPayload tp;
    tp.sub      = std::to_string(user->id);
    tp.email    = email;
    tp.provider = "password";
    tp.jti      = SessionManager::generateJti();
    std::string accessToken = m_token.generateAccess(tp, 900);
    bindSession(tp.sub, tp.jti);  // 单设备登录

    // 清除旧 refresh token，只保留最新一个
    SessionManager::revokeAllForUser(user->id, &m_pool);
    std::string refreshToken = TokenService::generateRefresh();
    SessionManager::storeRefresh(refreshToken, tp.sub, email, "password", 30*24*3600, &m_pool);

    std::ostringstream resp;
    resp << "{\"success\":true,\"message\":\"欢迎回来！塔坦已经迫不及待想和你开一局了~\""
         << ",\"access_token\":\"" << jsonEscape(accessToken) << "\""
         << ",\"refresh_token\":\"" << jsonEscape(refreshToken) << "\""
         << ",\"token_type\":\"Bearer\""
         << ",\"expires_in\":900"
         << ",\"uid\":\"" << std::to_string(user->id) << "\""
         << ",\"email\":\"" << jsonEscape(email) << "\""
         << ",\"username\":\"" << jsonEscape(user->username) << "\""
         << "}";
    return resp.str();
}

// ── loginWithCode ────────────────────────────────────
std::string AuthService::loginWithCode(const std::string &email,
                                        const std::string &code) {
    if (email.empty() || code.empty())
        return jsonErr("FIELDS_REQUIRED", "请输入邮箱和验证码");
    if (!validEmail(email))
        return jsonErr("INVALID_EMAIL", "邮箱格式不正确");
    if (!m_users.exists(email))
        return jsonErr("ACCOUNT_NOT_FOUND", "该账号不存在，请先注册");
    if (!m_codes.validate(email, code, CodePurpose::LOGIN))
        return jsonErr("INVALID_CODE", "验证码错误或已过期");

    auto user = m_users.findByEmail(email);
    if (!user) return jsonErr("DB_ERROR", "用户数据异常");

    TokenPayload tp;
    tp.sub      = std::to_string(user->id);
    tp.email    = email;
    tp.provider = "password";
    tp.jti      = SessionManager::generateJti();
    std::string accessToken  = m_token.generateAccess(tp, 900);
    bindSession(tp.sub, tp.jti);
    std::string refreshToken = TokenService::generateRefresh();
    SessionManager::revokeAllForUser(user->id, &m_pool);
    SessionManager::storeRefresh(refreshToken, tp.sub, email, "password", 30*24*3600, &m_pool);

    std::ostringstream resp;
    resp << "{\"success\":true,\"message\":\"欢迎回来！塔坦已经迫不及待想和你开一局了~\""
         << ",\"access_token\":\"" << jsonEscape(accessToken) << "\""
         << ",\"refresh_token\":\"" << jsonEscape(refreshToken) << "\""
         << ",\"token_type\":\"Bearer\""
         << ",\"expires_in\":900"
         << ",\"uid\":\"" << std::to_string(user->id) << "\""
         << ",\"email\":\"" << jsonEscape(email) << "\""
         << ",\"username\":\"" << jsonEscape(user->username) << "\""
         << "}";
    return resp.str();
}

// ── register ────────────────────────────────────────
std::string AuthService::doRegister(const std::string &email, const std::string &password,
                                     const std::string &code, const std::string &username) {
    if (email.empty() || password.empty() || code.empty())
        return jsonErr("FIELDS_REQUIRED", "请填写邮箱、密码和验证码");
    if (!validEmail(email))
        return jsonErr("INVALID_EMAIL", "邮箱格式不正确");
    if (!validPassword(password))
        return jsonErr("WEAK_PASSWORD", "密码需为6-20位，且包含字母和数字");

    if (m_users.exists(email))
        return jsonErr("EMAIL_EXISTS", "该邮箱已被注册，请直接登录");

    if (!m_codes.validate(email, code, CodePurpose::REGISTER))
        return jsonErr("INVALID_CODE", "验证码错误或已过期");

    if (!m_users.create(email, password, username))
        return jsonErr("REGISTER_FAILED", "注册失败，请稍后重试");

    return jsonOk("塔坦开心得转起了圈！欢迎新战友加入我们的大家庭~", "email", email);
}

// ── resetPassword ───────────────────────────────────
std::string AuthService::resetPassword(const std::string &email, const std::string &code,
                                        const std::string &newPassword) {
    if (email.empty() || code.empty() || newPassword.empty())
        return jsonErr("FIELDS_REQUIRED", "请填写邮箱、验证码和新密码");
    if (!validEmail(email))
        return jsonErr("INVALID_EMAIL", "邮箱格式不正确");
    if (!validPassword(newPassword))
        return jsonErr("WEAK_PASSWORD", "密码需为6-20位，且包含字母和数字");

    if (!m_users.exists(email))
        return jsonErr("ACCOUNT_NOT_FOUND", "该账号不存在，请先注册");

    if (!m_codes.validate(email, code, CodePurpose::RESET)) {
        return jsonErr("INVALID_CODE", "验证码错误或已过期");
    }

    if (!m_users.updatePassword(email, newPassword)) {
        return jsonErr("RESET_FAILED", "密码重置失败，请稍后重试");
    }

    auto user = m_users.findByEmail(email);
    if (user) SessionManager::revokeAllForUser(user->id, &m_pool);

    return jsonOk("哈哒帮你把密码锁修好啦，这次可别再弄丢了哦~");
}

// ── OAuth ─────────────────────────────────────────────
std::string AuthService::oauthGetUrl(const std::string &provider,
                                      std::string &state) {
    if (provider == "github")
        return m_oauth->githubAuthUrl(state);
    if (provider == "discord")
        return m_oauth->discordAuthUrl(state);
    return {};
}

std::string AuthService::oauthCallback(const std::string &provider,
                                        const std::string &code,
                                        const std::string &state) {
    std::optional<OAuthUser> oauthUser;
    if (provider == "github")
        oauthUser = m_oauth->githubExchange(code, state);
    else if (provider == "discord")
        oauthUser = m_oauth->discordExchange(code, state);

    if (!oauthUser) {
        return jsonErr("OAUTH_FAILED", "第三方授权失败，请重试");
    }

    auto &ou = *oauthUser;
    auto existing = m_users.findByOAuth(ou.provider, ou.providerId);
    std::string email;

    if (existing) {
        email = existing->email;
    } else if (!ou.email.empty() && m_users.exists(ou.email)) {
        m_users.linkOAuth(ou.email, ou.provider, ou.providerId);
        email = ou.email;
    } else {
        email = ou.email.empty()
            ? (ou.name + "@" + ou.provider + ".user")
            : ou.email;
        m_users.createFromOAuth(email, ou.name, ou.provider, ou.providerId);
    }

    auto user = m_users.findByEmail(email);
    if (!user) return jsonErr("DB_ERROR", "用户创建失败，请重试");

    TokenPayload tp;
    tp.sub      = std::to_string(user->id);
    tp.email    = email;
    tp.provider = ou.provider;
    tp.jti      = SessionManager::generateJti();
    std::string accessToken  = m_token.generateAccess(tp, 900);
    bindSession(tp.sub, tp.jti);
    std::string refreshToken = TokenService::generateRefresh();
    SessionManager::revokeAllForUser(user->id, &m_pool);
    SessionManager::storeRefresh(refreshToken, tp.sub, email, ou.provider, 30*24*3600, &m_pool);

    std::ostringstream resp;
    resp << "{\"success\":true,\"message\":\"欢迎回来！塔坦已经迫不及待想和你开一局了~\""
         << ",\"access_token\":\"" << jsonEscape(accessToken) << "\""
         << ",\"refresh_token\":\"" << jsonEscape(refreshToken) << "\""
         << ",\"token_type\":\"Bearer\""
         << ",\"expires_in\":900"
         << ",\"uid\":\"" << std::to_string(user->id) << "\""
         << ",\"email\":\"" << jsonEscape(email) << "\""
         << ",\"username\":\"" << jsonEscape(user->username) << "\""
         << ",\"provider\":\"" << jsonEscape(ou.provider) << "\""
         << ",\"name\":\"" << jsonEscape(ou.name) << "\""
         << ",\"avatar\":\"" << jsonEscape(ou.avatarUrl) << "\""
         << "}";
    return resp.str();
}

// ── OAuth 绑定模式 ──
std::string AuthService::oauthCallbackBind(const std::string &provider,
                                            const std::string &code,
                                            const std::string &state,
                                            uint64_t bindUserId) {
    std::optional<OAuthUser> oauthUser;
    if (provider == "github")
        oauthUser = m_oauth->githubExchange(code, state);
    else if (provider == "discord")
        oauthUser = m_oauth->discordExchange(code, state);

    if (!oauthUser)
        return jsonErr("OAUTH_FAILED", "第三方授权失败，请重试");

    auto &ou = *oauthUser;
    auto user = m_users.findById(bindUserId);
    if (!user)
        return jsonErr("USER_NOT_FOUND", "用户不存在");

    m_users.linkOAuth(user->email, ou.provider, ou.providerId);

    TokenPayload tp;
    tp.sub      = std::to_string(user->id);
    tp.email    = user->email;
    tp.provider = ou.provider;
    tp.jti      = SessionManager::generateJti();
    std::string accessToken  = m_token.generateAccess(tp, 900);
    bindSession(tp.sub, tp.jti);
    std::string refreshToken = TokenService::generateRefresh();
    SessionManager::storeRefresh(refreshToken, tp.sub, tp.email, ou.provider, 30*24*3600, &m_pool);

    std::ostringstream resp;
    resp << "{\"success\":true,\"message\":\"绑定成功\""
         << ",\"access_token\":\"" << jsonEscape(accessToken) << "\""
         << ",\"refresh_token\":\"" << jsonEscape(refreshToken) << "\""
         << ",\"token_type\":\"Bearer\""
         << ",\"expires_in\":900"
         << ",\"uid\":\"" << std::to_string(user->id) << "\""
         << ",\"email\":\"" << jsonEscape(user->email) << "\""
         << ",\"username\":\"" << jsonEscape(user->username) << "\""
         << ",\"avatar\":\"" << jsonEscape(ou.avatarUrl) << "\""
         << "}";
    return resp.str();
}

// ── 鉴权 ─────────────────────────────────────────────

std::string AuthService::extractBearerToken(const std::string &headers) {
    // 在原始 HTTP headers 中查找 "Authorization: Bearer <token>"
    // headers 格式: "Host: x\r\nAuthorization: Bearer abc\r\n..."
    std::string key = "Authorization:";
    auto pos = headers.find(key);
    if (pos == std::string::npos) {
        // 也支持小写开头
        key = "authorization:";
        pos = headers.find(key);
    }
    if (pos == std::string::npos) {
        return {};
    }

    pos += key.size();
    // 跳过空格
    while (pos < headers.size() && (headers[pos] == ' ' || headers[pos] == '\t'))
        ++pos;

    // 期望 "Bearer " 前缀
    std::string bearer = "Bearer ";
    if (headers.compare(pos, bearer.size(), bearer) != 0) {
        // 也支持小写
        bearer = "bearer ";
        if (headers.compare(pos, bearer.size(), bearer) != 0)
            return {};
    }
    pos += bearer.size();

    // 读取 token 直到 \r 或 \n
    auto end = headers.find_first_of("\r\n", pos);
    std::string token = headers.substr(pos, end - pos);
    // trim trailing whitespace
    while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
        token.pop_back();
    return token;
}

std::optional<TokenPayload> AuthService::authenticate(
    const std::string &headers) const {
    std::string token = extractBearerToken(headers);
    if (token.empty()) return std::nullopt;
    auto tp = m_token.verify(token);
    if (!tp) return std::nullopt;

    // ── 单设备登录: 检查 JTI ──
    try {
        auto &r = RedisClient::instance();
        auto stored = r.get("session:" + tp->sub);
        if (stored && *stored != tp->jti) {
            // 另一设备已登录, 此 token 作废
            return std::nullopt;
        }
    } catch (...) {}

    return tp;
}

void AuthService::bindSession(const std::string &uid, const std::string &jti) {
    try {
        RedisClient::instance().setex("session:" + uid, 900, jti);
    } catch (...) {}
}

// ── validators ──────────────────────────────────────
bool AuthService::validEmail(const std::string &email) const {
    auto at = email.find('@');
    if (at == std::string::npos || at == 0) return false;
    auto dot = email.find('.', at + 1);
    return dot != std::string::npos && dot > at + 1 && dot < email.size() - 1;
}

bool AuthService::validPassword(const std::string &pw) const {
    if (pw.size() < ServerConfig::MIN_PWD_LEN || pw.size() > ServerConfig::MAX_PWD_LEN)
        return false;
    if (pw.find(' ') != std::string::npos) return false;
    bool hasL = false, hasD = false;
    for (char c : pw) {
        if (std::isalpha(static_cast<unsigned char>(c))) hasL = true;
        if (std::isdigit(static_cast<unsigned char>(c))) hasD = true;
    }
    return hasL && hasD;
}
