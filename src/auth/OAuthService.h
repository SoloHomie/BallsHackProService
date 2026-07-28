#pragma once
#include <string>
#include <optional>

// Data returned by OAuth provider after code exchange
struct OAuthUser {
    std::string provider;     // "github" | "discord"
    std::string providerId;   // provider-side user id
    std::string email;
    std::string name;
    std::string avatarUrl;
    std::string accessToken;  // 第三方返回的 access token
    std::string refreshToken; // 第三方返回的 refresh token（可能为空）
    std::string rawJson;      // 第三方返回的原始用户 JSON
};

class OAuthService {
public:
    OAuthService();
    ~OAuthService();

    // ── GitHub OAuth ───────────────────────────────
    std::string githubAuthUrl(std::string &outState) const;
    std::optional<OAuthUser> githubExchange(const std::string &code,
                                            const std::string &state) const;

    // ── Discord OAuth ──────────────────────────────
    std::string discordAuthUrl(std::string &outState) const;
    std::optional<OAuthUser> discordExchange(const std::string &code,
                                             const std::string &state) const;

private:
    // Generic HTTP helpers (use libcurl)
    std::string httpPost(const std::string &url,
                         const std::string &body,
                         const std::string &contentType,
                         const std::string &authHeader) const;
    std::string httpGet(const std::string &url,
                        const std::string &authHeader) const;

    // Simple JSON field extractor
    static std::string jsonVal(const std::string &json, const std::string &key);

    // CSRF state token (simple random string)
    static std::string randomState();
};
