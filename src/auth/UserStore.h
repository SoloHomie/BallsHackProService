#pragma once
#include <string>
#include <unordered_map>
#include <optional>
#include <vector>
#include <cstdint>

class DbPool;

class UserStore {
public:
    struct UserInfo {
        uint64_t    id = 0;
        std::string email;
        std::string username;
        std::string passwordHash;   // PBKDF2 hex（不再暴露，但保留兼容）
        std::string createdAt;
        std::string subscription_expires;  // NULL if not subscribed
        // OAuth bindings: provider → providerId
        std::unordered_map<std::string, std::string> oauthLinks;
    };

    explicit UserStore(DbPool& pool) : m_pool(pool) {}

    // ── Password-based ──────────────────────────────
    bool exists(const std::string &email) const;
    std::optional<UserInfo> findByEmail(const std::string &email) const;

    bool create(const std::string &email, const std::string &password,
                const std::string &username = {});
    bool updatePassword(const std::string &email, const std::string &newPassword);
    bool verifyPassword(const std::string &email, const std::string &password) const;

    // ── OAuth-based ─────────────────────────────────
    std::optional<UserInfo> findByOAuth(const std::string &provider,
                                        const std::string &providerId) const;
    bool linkOAuth(const std::string &email,
                   const std::string &provider, const std::string &providerId);
    bool unlinkOAuth(const std::string &email, const std::string &provider);
    // 返回 provider → {provider_id, provider_username}
    std::vector<std::tuple<std::string,std::string,std::string>>
        listOAuthLinks(const std::string &email) const;
    std::optional<UserInfo> findById(uint64_t id) const;
    bool createFromOAuth(const std::string &email,
                         const std::string &name,
                         const std::string &provider,
                         const std::string &providerId);

    int count() const;

private:
    DbPool& m_pool;
};
