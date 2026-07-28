#pragma once
// ── PBKDF2-HMAC-SHA256 密码哈希（OpenSSL，跨平台）──────
// 公式:  hex(独立盐) + 密码原文 + uid → PBKDF2(60万轮) → hex 输出
//
// generateSalt()   → 32 字节随机盐的 hex（64 字符），每用户唯一
// hash(salt,pw,uid) → 拼接后 PBKDF2 迭代 60 万次，返回 64 字符 hex

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <string>
#include <cstdint>
#include <cstring>

namespace PasswordHash {
    constexpr int  ITERATIONS = 600000;   // NIST 推荐值
    constexpr int  KEY_LEN    = 32;       // SHA-256 输出 32 字节

    // ── 生成随机盐（返回 64 字符 hex）───────────────────
    inline std::string generateSalt() {
        unsigned char buf[32];
        RAND_bytes(buf, sizeof(buf));
        static const char hex[] = "0123456789abcdef";
        std::string s(64, '\0');
        for (int i = 0; i < 32; ++i) {
            s[i * 2]     = hex[buf[i] >> 4];
            s[i * 2 + 1] = hex[buf[i] & 0xF];
        }
        return s;
    }

    // ── 哈希：hex(盐) + 密码 + uid → PBKDF2 → hex ──────
    inline std::string hash(const std::string& saltHex,
                            const std::string& password,
                            uint64_t uid) {
        // 拼接原料
        std::string material = saltHex + password + std::to_string(uid);

        // PKCS5_PBKDF2_HMAC (OpenSSL)
        unsigned char derived[KEY_LEN];
        int ret = PKCS5_PBKDF2_HMAC(
            material.data(), (int)material.size(),
            reinterpret_cast<const unsigned char*>(saltHex.data()),
            (int)saltHex.size(),
            ITERATIONS,
            EVP_sha256(),
            KEY_LEN, derived);

        if (ret != 1) return {};  // 失败返回空

        // hex 编码
        static const char hex[] = "0123456789abcdef";
        std::string result(KEY_LEN * 2, '\0');
        for (int i = 0; i < KEY_LEN; ++i) {
            result[i * 2]     = hex[derived[i] >> 4];
            result[i * 2 + 1] = hex[derived[i] & 0xF];
        }
        return result;
    }

    // ── 校验密码 ─────────────────────────────────────────
    inline bool verify(const std::string& saltHex,
                       const std::string& password,
                       uint64_t uid,
                       const std::string& storedHash) {
        return hash(saltHex, password, uid) == storedHash;
    }
}
