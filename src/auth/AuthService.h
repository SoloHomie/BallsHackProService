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

    std::string sendCode(const std::string &email, const std::string &purpose);
    std::string login(const std::string &email, const std::string &password);
    std::string loginWithCode(const std::string &email, const std::string &code);
    std::string doRegister(const std::string &email, const std::string &password,
                           const std::string &code, const std::string &username = {});
    std::string resetPassword(const std::string &email, const std::string &code,
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
    // 从 HTTP headers 提取 Authorization: Bearer <token> 并验证 JWT
    std::optional<TokenPayload> authenticate(const std::string &headers) const;
    // 从 headers 裸字符串中提取 Bearer token
    static std::string extractBearerToken(const std::string &headers);

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
