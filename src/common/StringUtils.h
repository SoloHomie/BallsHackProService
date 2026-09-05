#pragma once
// ── String utilities (header-only) ────────────────────
// Lightweight helpers for JSON/URL parsing used by
// route handlers and template rendering.
#include <string>

namespace StringUtils {

/// Extract a top-level JSON value for a given key — string, number, or boolean.
/// Returns empty string if key is not found or JSON is invalid.
/// 实现见 StringUtils.cpp (2026-09-06 起用 nlohmann/json 解析, 替换手写子串匹配)
std::string jsonVal(const std::string &json, const std::string &key);

/// URL-decode %XX and '+' sequences per RFC 3986 / form-urlencoded.
inline std::string urlDecode(const std::string &s) {
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) { r += char(hi * 16 + lo); i += 2; continue; }
        }
        if (s[i] == '+') r += ' ';
        else r += s[i];
    }
    return r;
}

/// Extract a query parameter value from a URL query string.
inline std::string queryVal(const std::string &query, const std::string &key) {
    std::string pat = key + "=";
    auto pos = query.find(pat);
    if (pos == std::string::npos) return {};
    pos += pat.size();
    auto end = query.find('&', pos);
    if (end == std::string::npos) end = query.size();
    return urlDecode(query.substr(pos, end - pos));
}

/// URL-encode a string per RFC 3986 (reserved chars only).
inline std::string urlEncode(const std::string &s) {
    static const char *hex = "0123456789ABCDEF";
    std::string r;
    r.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') ||
            ('0' <= c && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            r += (char)c;
        } else {
            r += '%';
            r += hex[c >> 4];
            r += hex[c & 15];
        }
    }
    return r;
}

/// HTML-escape special characters for safe embedding in HTML attributes.
inline std::string htmlEscape(const std::string &s) {
    std::string r;
    r.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
        case '&':  r += "&amp;"; break;
        case '"':  r += "&quot;"; break;
        case '<':  r += "&lt;"; break;
        case '>':  r += "&gt;"; break;
        case '\'': r += "&#39;"; break;
        default:   r += c;
        }
    }
    return r;
}

/// Replace all occurrences of `key` with `value` in `s`.
inline std::string replace(const std::string &s,
                           const std::string &key,
                           const std::string &value) {
    std::string r;
    r.reserve(s.size());
    size_t pos = 0;
    while (true) {
        auto found = s.find(key, pos);
        if (found == std::string::npos) {
            r.append(s, pos, s.size() - pos);
            break;
        }
        r.append(s, pos, found - pos);
        r.append(value);
        pos = found + key.size();
    }
    return r;
}

} // namespace StringUtils
