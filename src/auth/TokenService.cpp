#include "TokenService.h"
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <sstream>
#include <cstdint>
#include <cstring>
#include <vector>
#include <mutex>

TokenService::TokenService(const std::string &secret) : m_secret(secret) {}

// ── JSON escape ──────────────────────────────────────
std::string TokenService::jsonEscape(const std::string &s) {
    std::string r;
    r.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
        case '"':  r += "\\\""; break;
        case '\\': r += "\\\\"; break;
        default:   r += c;
        }
    }
    return r;
}

// ── Base64 URL-encode (no padding, - instead of +, _ instead of /) ─
std::string TokenService::base64UrlEncode(const std::string &data) {
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    // Process 3 bytes at a time → 4 base64 chars (standard, no overflow)
    for (size_t i = 0; i < data.size(); i += 3) {
        unsigned char b0 = (unsigned char)data[i];
        unsigned char b1 = (i + 1 < data.size()) ? (unsigned char)data[i + 1] : 0;
        unsigned char b2 = (i + 2 < data.size()) ? (unsigned char)data[i + 2] : 0;
        uint32_t val = ((uint32_t)b0 << 16) | ((uint32_t)b1 << 8) | (uint32_t)b2;
        out += T[(val >> 18) & 0x3F];
        out += T[(val >> 12) & 0x3F];
        out += (i + 1 < data.size()) ? T[(val >> 6) & 0x3F] : '=';
        out += (i + 2 < data.size()) ? T[val & 0x3F] : '=';
    }
    while (!out.empty() && out.back() == '=') out.pop_back();  // strip padding
    // Replace +/ → -_
    for (char &c : out) { if (c == '+') c = '-'; if (c == '/') c = '_'; }
    return out;
}

// ── Base64 URL-decode ────────────────────────────────
static void initBase64Decode(int D[256]) {
    for (int i = 0; i < 256; ++i) D[i] = -1;
    for (int i = 0; i < 64; ++i)
        D[(int)"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "abcdefghijklmnopqrstuvwxyz0123456789+/"[i]] = i;
}

std::string TokenService::base64UrlDecode(const std::string &data) {
    static int D[256] = {};
    static std::once_flag initFlag;
    std::call_once(initFlag, []() { initBase64Decode(D); });
    std::string s = data;
    for (char &c : s) { if (c == '-') c = '+'; if (c == '_') c = '/'; }
    // Add padding
    while (s.size() % 4) s += '=';
    std::string out;
    out.reserve((s.size() / 4) * 3);
    // Process 4 chars at a time → 3 bytes (standard, no overflow)
    for (size_t i = 0; i < s.size(); i += 4) {
        int v0 = D[(unsigned char)s[i]];
        int v1 = D[(unsigned char)s[i + 1]];
        int v2 = (i + 2 < s.size() && s[i + 2] != '=') ? D[(unsigned char)s[i + 2]] : -1;
        int v3 = (i + 3 < s.size() && s[i + 3] != '=') ? D[(unsigned char)s[i + 3]] : -1;
        if (v0 == -1 || v1 == -1) break;
        uint32_t val = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12);
        out += (unsigned char)((val >> 16) & 0xFF);
        if (v2 != -1) {
            val |= ((uint32_t)v2 << 6);
            out += (unsigned char)((val >> 8) & 0xFF);
            if (v3 != -1) {
                val |= (uint32_t)v3;
                out += (unsigned char)(val & 0xFF);
            }
        }
    }
    return out;
}

// ── HMAC-SHA256 via OpenSSL ──────────────────────────
std::string TokenService::hmacSha256(const std::string &key,
                                      const std::string &msg) {
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha256(),
         key.data(), (int)key.size(),
         reinterpret_cast<const unsigned char*>(msg.data()), msg.size(),
         result, &len);
    return std::string(reinterpret_cast<char*>(result), len);
}

// ── Generate JWT Access Token ─────────────────────────
std::string TokenService::generateAccess(const TokenPayload &payload,
                                          int ttlSeconds) const {
    std::ostringstream header;
    header << "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
    std::string h = base64UrlEncode(header.str());

    auto now = std::chrono::system_clock::now();
    auto iat = std::chrono::duration_cast<std::chrono::seconds>(
                   now.time_since_epoch()).count();
    auto exp = iat + ttlSeconds;

    std::ostringstream body;
    body << "{\"sub\":\"" << jsonEscape(payload.sub) << "\""
         << ",\"email\":\"" << jsonEscape(payload.email) << "\""
         << ",\"provider\":\"" << jsonEscape(payload.provider) << "\""
         << ",\"jti\":\"" << jsonEscape(payload.jti) << "\""
         << ",\"iat\":" << iat
         << ",\"exp\":" << exp << "}";
    std::string p = base64UrlEncode(body.str());

    std::string sig = base64UrlEncode(hmacSha256(m_secret, h + "." + p));

    return h + "." + p + "." + sig;
}

// ── Generate Refresh Token（随机 64 字符 hex）─────────
std::string TokenService::generateRefresh() {
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

// ── Verify JWT ───────────────────────────────────────
std::optional<TokenPayload> TokenService::verify(
    const std::string &token) const {
    auto dot1 = token.find('.');
    if (dot1 == std::string::npos) return {};
    auto dot2 = token.find('.', dot1 + 1);
    if (dot2 == std::string::npos) return {};

    std::string h   = token.substr(0, dot1);
    std::string p   = token.substr(dot1 + 1, dot2 - dot1 - 1);
    std::string sig = token.substr(dot2 + 1);

    // Verify signature
    std::string expected = base64UrlEncode(hmacSha256(m_secret, h + "." + p));
    if (sig != expected) return {};

    // Decode payload
    std::string json = base64UrlDecode(p);

    // Minimal JSON parse (no library)
    auto val = [&](const std::string &key) -> std::string {
        std::string pat = "\"" + key + "\":";
        auto pos = json.find(pat);
        if (pos == std::string::npos) return {};
        pos += pat.size();
        // String value
        if (json[pos] == '"') {
            pos++;
            auto end = json.find('"', pos);
            return json.substr(pos, end - pos);
        }
        // Numeric value
        auto end = json.find_first_of(",}", pos);
        return json.substr(pos, end - pos);
    };

    TokenPayload tp;
    tp.sub      = val("sub");
    tp.email    = val("email");
    tp.provider = val("provider");
    tp.jti      = val("jti");

    try {
        tp.iat = std::stoll(val("iat"));
        tp.exp = std::stoll(val("exp"));
    } catch (...) { return {}; }

    // Check expiration
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    if (now > tp.exp) return {};

    return tp;
}
