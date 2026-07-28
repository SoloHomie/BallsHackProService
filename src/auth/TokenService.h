#pragma once
#include <string>
#include <chrono>
#include <optional>

struct TokenPayload {
    std::string sub;      // user id
    std::string email;    // user email
    std::string provider; // "password" | "github" | "discord"
    std::string jti;      // JWT unique ID
    int64_t     iat;      // issued at (unix seconds)
    int64_t     exp;      // expiration (unix seconds)
};

class TokenService {
public:
    explicit TokenService(const std::string &secret);

    // ── Access Token (JWT) ───────────────────────────
    // 短期（默认 15 分钟），无状态，签名自验证
    std::string generateAccess(const TokenPayload &payload,
                               int ttlSeconds = 900) const;

    // 验证 JWT，返回 payload。过期/无效返回 nullopt
    std::optional<TokenPayload> verify(const std::string &token) const;

    // ── Refresh Token ────────────────────────────────
    // 随机 64 字符 hex 串，用作 Redis 查询 key
    static std::string generateRefresh();
    static constexpr int REFRESH_TTL_SEC = 30 * 24 * 3600; // 30 天

private:
    static std::string base64UrlEncode(const std::string &data);
    static std::string base64UrlDecode(const std::string &data);
    static std::string hmacSha256(const std::string &key, const std::string &msg);
    static std::string jsonEscape(const std::string &s);

    std::string m_secret;
};
