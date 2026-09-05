#pragma once
#include <string>
#include <memory>
#include "CodeManager.h"
#include "UserStore.h"
#include "TokenService.h"

class DbPool;
class EmailService;
class OAuthService;

class AuthService {
public:
    explicit AuthService(DbPool& pool);
    ~AuthService();

    // email 均按值传入并在入口归一化 (trim + 小写, 见 normalizeEmail)
    std::string sendCode(std::string email, const std::string &purpose);
    std::string login(std::string email, const std::string &password);
    std::string loginWithCode(std::string email, const std::string &code);
    std::string doRegister(std::string email, const std::string &password,
                           const std::string &code, const std::string &username = {});
    std::string resetPassword(std::string email, const std::string &code,
                              const std::string &newPassword);

    // ── OAuth ──────────────────────────────────────
    std::string oauthGetUrl(const std::string &provider, std::string &state);
    std::string oauthCallback(const std::string &provider,
                              const std::string &code, const std::string &state);
    // 绑定模式：直接绑 OAuth 到指定用户（不按邮箱匹配）
    std::string oauthCallbackBind(const std::string &provider,
                                   const std::string &code,
                                   const std::string &state,
                                   uint64_t bindUserId);

    CodeManager  &codeManager()  { return m_codes; }
    UserStore    &userStore()    { return m_users; }
    TokenService &tokenService() { return m_token; }

    // ── 鉴权 ──────────────────────────────────────
    std::optional<TokenPayload> authenticate(const std::string &headers) const;
    // 仅验 JWT 签名 (不做会话/封禁检查) — 心跳路由用于区分 被踢/被封/顶号
    std::optional<TokenPayload> peekToken(const std::string &headers) const;
    static std::string extractBearerToken(const std::string &headers);
    // 绑定会话: 新登录时调用, 使旧设备 token 失效
    void bindSession(const std::string &uid, const std::string &jti);
    // subscribed + subscription_expires JSON fields (MySQL direct query) for login/refresh responses
    std::string subFieldsJson(const std::string &email) const;

    // 邮箱归一化: 去首尾空白 + 小写 (复制粘贴常带尾部换行, 会导致 exists 查无账号)
    static std::string normalizeEmail(const std::string &email);

    // ── JSON response builders ─────────────────────
    static std::string jsonOk(const std::string &msg);
    static std::string jsonOk(const std::string &msg,
                              const std::string &k, const std::string &v);
    static std::string jsonErr(const std::string &errcode, const std::string &msg);
    static std::string jsonEscape(const std::string &s);

private:
    bool validEmail(const std::string &email) const;
    bool validPassword(const std::string &pw) const;

    CodeManager m_codes;
    DbPool&     m_pool;
    UserStore   m_users;
    std::unique_ptr<EmailService> m_email;
    std::unique_ptr<OAuthService>  m_oauth;
    TokenService m_token;
};
