#include "CodeManager.h"
#include "../core/config/ServerConfig.h"
#include "../core/cache/RedisClient.h"
#include <iostream>
#include <openssl/rand.h>

std::string CodeManager::randomCode() {
    // Cryptographically secure 6-digit code via OpenSSL RAND_bytes
    unsigned char buf[ServerConfig::CODE_LENGTH];
    if (RAND_bytes(buf, sizeof(buf)) != 1) {
        // Fallback: extremely unlikely, but handle gracefully
        return {};
    }
    std::string code;
    code.reserve(ServerConfig::CODE_LENGTH);
    for (int i = 0; i < ServerConfig::CODE_LENGTH; ++i)
        code.push_back(static_cast<char>('0' + (buf[i] % 10)));
    return code;
}

std::string CodeManager::generate(const std::string &email,
                                  const std::string &purpose) {
    if (!canSend(email, purpose)) {
        return {};
    }

    auto &r = RedisClient::instance();
    std::string code;
    int tries = 0;

    // Delete old code if exists (from a previous failed send)
    try { r.del(codeKey(email, purpose)); } catch (...) {}

    try {
    do {
        code = randomCode();
        if (++tries > 20) {
            return {};
        }
    } while (r.exists(usedKey(code)));
    } catch (const std::exception &) {
        return {};
    } catch (...) {
        return {};
    }

    // Store code with auto-expiry (with fallback)
    try {
        r.setex(codeKey(email, purpose), ServerConfig::CODE_TTL_SEC, code);
        r.setex(cdKey(email, purpose), ServerConfig::CODE_COOLDOWN, "1");
    } catch (const std::exception &) {
        // continue — code is still valid even if Redis storage failed
    } catch (...) {
    }
    return code;
}

bool CodeManager::validate(const std::string &email, const std::string &code,
                            const std::string &purpose) {
    try {
        auto &r = RedisClient::instance();
        std::string key = codeKey(email, purpose);
        auto saved = r.get(key);
        if (!saved) {
            return false;
        }
        if (*saved != code) {
            return false;
        }

        // One-time use: delete code + cooldown, mark as used
        r.del(key);
        r.del(cdKey(email, purpose));  // 成功后清除冷却，允许立即再次发送
        r.setex(usedKey(code), 60, "1");  // 60s 防重复提交即可
        return true;
    } catch (const std::exception &) {
        return false;
    } catch (...) {
        return false;
    }
}

bool CodeManager::canSend(const std::string &email,
                           const std::string &purpose) const {
    try {
        return !RedisClient::instance().exists(cdKey(email, purpose));
    } catch (const std::exception &) {
        return false; // fail-closed: deny sending if Redis is unreachable
    } catch (...) {
        return false;
    }
}

int CodeManager::cooldownRemaining(const std::string &email,
                                    const std::string &purpose) const {
    try {
        auto &r = RedisClient::instance();
        long long ttl = r.ttl(cdKey(email, purpose));
        return ttl > 0 ? static_cast<int>(ttl) : 0;
    } catch (const std::exception &) {
        return 0;
    } catch (...) {
        return 0;
    }
}

void CodeManager::purgeExpired() {
    // Redis keys auto-expire — nothing to do
}
