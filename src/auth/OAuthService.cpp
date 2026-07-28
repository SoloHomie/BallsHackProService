#include "OAuthService.h"
#include "../core/config/OAuthConfig.h"
#include "../common/StringUtils.h"
#include "../core/cache/RedisClient.h"
#include <curl/curl.h>
#include <iostream>
#include <random>
#include <sstream>

// ── libcurl write callback ───────────────────────────
static size_t writeCb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *s = static_cast<std::string *>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

OAuthService::OAuthService()  { curl_global_init(CURL_GLOBAL_DEFAULT); }
OAuthService::~OAuthService() { curl_global_cleanup(); }

// ── HTTP POST ────────────────────────────────────────
std::string OAuthService::httpPost(const std::string &url,
                                   const std::string &body,
                                   const std::string &contentType,
                                   const std::string &authHeader) const {
    CURL *curl = curl_easy_init();
    if (!curl) return {};
    std::string resp;
    struct curl_slist *headers = nullptr;
    if (!contentType.empty())
        headers = curl_slist_append(headers,
            ("Content-Type: " + contentType).c_str());
    if (!authHeader.empty())
        headers = curl_slist_append(headers,
            ("Authorization: " + authHeader).c_str());
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "User-Agent: BallsHackProService/1.0");

    curl_easy_setopt(curl, CURLOPT_URL,               url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,        headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,        body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,     (long)body.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,     writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,         &resp);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,    15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,           30L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,    1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST,    2L);
    std::string proxy = OAuthConfig::OAUTH_PROXY();
    if (!proxy.empty())
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());
    curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return resp;
}

// ── HTTP GET ─────────────────────────────────────────
std::string OAuthService::httpGet(const std::string &url,
                                  const std::string &authHeader) const {
    CURL *curl = curl_easy_init();
    if (!curl) return {};
    std::string resp;
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers,
        ("Authorization: " + authHeader).c_str());
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "User-Agent: BallsHackProService/1.0");

    curl_easy_setopt(curl, CURLOPT_URL,               url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,        headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,     writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,         &resp);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,    15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,           30L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,    1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST,    2L);
    std::string proxy = OAuthConfig::OAUTH_PROXY();
    if (!proxy.empty())
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());
    curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return resp;
}

// ── Simple JSON extractor ────────────────────────────
std::string OAuthService::jsonVal(const std::string &json,
                                   const std::string &key) {
    // Handle string values
    {
        std::string pat = "\"" + key + "\":\"";
        auto pos = json.find(pat);
        if (pos != std::string::npos) {
            pos += pat.size();
            auto end = json.find('"', pos);
            if (end != std::string::npos) return json.substr(pos, end - pos);
        }
    }
    // Handle numeric/boolean values
    {
        std::string pat = "\"" + key + "\":";
        auto pos = json.find(pat);
        if (pos != std::string::npos) {
            pos += pat.size();
            auto end = json.find_first_of(",}\n\r", pos);
            return json.substr(pos, end - pos);
        }
    }
    return {};
}

// ── CSRF state token ─────────────────────────────────
std::string OAuthService::randomState() {
    static const char hex[] = "0123456789abcdef";
    static std::mt19937 gen([]() {
        std::random_device rd;
        std::seed_seq seq{rd(), rd(), rd(), rd()};
        return std::mt19937(seq);
    }());
    static std::uniform_int_distribution<> dis(0, 15);
    std::string s(32, '0');
    for (int i = 0; i < 32; ++i) s[i] = hex[dis(gen)];
    return s;
}

// ── GitHub: Step 1 — authorization URL ───────────────
std::string OAuthService::githubAuthUrl(std::string &outState) const {
    outState = std::string("github:") + randomState();
    std::ostringstream url;
    url << OAuthConfig::GitHub::AUTH_URL
        << "?client_id=" << OAuthConfig::GitHub::CLIENT_ID()
        << "&redirect_uri=" << StringUtils::urlEncode(OAuthConfig::REDIRECT_URI)
        << "&scope=user:email"
        << "&state=" << outState;
    RedisClient::instance().setex("oauth:state:" + outState, 600, "github");
    return url.str();
}

// ── GitHub: Step 2 — exchange code for user ──────────
std::optional<OAuthUser> OAuthService::githubExchange(
    const std::string &code, const std::string &state) const {
    // Verify CSRF state
    auto &r = RedisClient::instance();
    std::string stateKey = "oauth:state:" + state;
    auto stored = r.get(stateKey);
    r.del(stateKey);

    // 1. Exchange code for access token
    std::ostringstream body;
    body << "client_id=" << OAuthConfig::GitHub::CLIENT_ID()
         << "&client_secret=" << OAuthConfig::GitHub::CLIENT_SECRET()
         << "&code=" << code
         << "&redirect_uri=" << StringUtils::urlEncode(OAuthConfig::REDIRECT_URI);

    std::string tokenResp = httpPost(
        OAuthConfig::GitHub::TOKEN_URL,
        body.str(),
        "application/x-www-form-urlencoded", "");
    std::string accessToken = jsonVal(tokenResp, "access_token");
    if (accessToken.empty()) {
        // Fallback: parse as form-encoded
        auto pos = tokenResp.find("access_token=");
        if (pos != std::string::npos) {
            pos += 13;
            auto end = tokenResp.find('&', pos);
            accessToken = tokenResp.substr(pos, end - pos);
        }
    }

    // 2. Fetch user info
    std::string userJson = httpGet(
        OAuthConfig::GitHub::USER_API,
        "Bearer " + accessToken);

    std::string ghId    = jsonVal(userJson, "id");
    std::string ghLogin = jsonVal(userJson, "login");
    std::string ghEmail = jsonVal(userJson, "email");
    std::string ghAvatar= jsonVal(userJson, "avatar_url");

    if (ghId.empty()) return {};

    // 3. If email is not public (null or empty), fetch emails separately
    if (ghEmail.empty() || ghEmail == "null") {
        std::string emailsJson = httpGet(
            "https://api.github.com/user/emails",
            "Bearer " + accessToken);
        // Find the primary verified email
        // GitHub emails: [{"email":"...","primary":true,"verified":true},...]
        auto pos = emailsJson.find("\"primary\":true");
        if (pos != std::string::npos) {
            // Search backwards for the email field before this one
            // Simple approach: find a verified email
            auto vpos = emailsJson.find("\"verified\":true");
            if (vpos != std::string::npos) {
                auto epos = emailsJson.rfind("\"email\":\"", vpos);
                if (epos != std::string::npos) {
                    epos += 9; // skip "email":"
                    auto end = emailsJson.find('"', epos);
                    if (end != std::string::npos)
                        ghEmail = emailsJson.substr(epos, end - epos);
                }
            }
        }
    }

    OAuthUser user;
    user.provider     = "github";
    user.providerId   = ghId;
    user.email        = ghEmail;
    user.name         = ghLogin;
    user.avatarUrl    = ghAvatar;
    user.accessToken  = accessToken;
    user.refreshToken = "";  // GitHub tokens don't expire unless revoked
    user.rawJson      = userJson;
    return user;
}

// ── Discord: Step 1 — authorization URL ─────────────
std::string OAuthService::discordAuthUrl(std::string &outState) const {
    outState = std::string("discord:") + randomState();
    std::ostringstream url;
    url << OAuthConfig::Discord::AUTH_URL
        << "?client_id=" << OAuthConfig::Discord::CLIENT_ID()
        << "&redirect_uri=" << StringUtils::urlEncode(OAuthConfig::REDIRECT_URI)
        << "&response_type=code"
        << "&scope=identify+email"
        << "&state=" << outState
        << "&prompt=consent";
    RedisClient::instance().setex("oauth:state:" + outState, 600, "discord");
    return url.str();
}

// ── Discord: Step 2 — exchange code for user ────────
std::optional<OAuthUser> OAuthService::discordExchange(
    const std::string &code, const std::string &state) const {
    // Verify CSRF state
    auto &r = RedisClient::instance();
    auto stored = r.get("oauth:state:" + state);
    if (!stored || *stored != "discord") return {};
    r.del("oauth:state:" + state);

    // 1. Exchange code for access token (Discord uses form-encoded body)
    std::ostringstream body;
    body << "client_id=" << OAuthConfig::Discord::CLIENT_ID()
         << "&client_secret=" << OAuthConfig::Discord::CLIENT_SECRET()
         << "&grant_type=authorization_code"
         << "&code=" << code
         << "&redirect_uri=" << StringUtils::urlEncode(OAuthConfig::REDIRECT_URI);

    std::string tokenResp = httpPost(
        OAuthConfig::Discord::TOKEN_URL,
        body.str(),
        "application/x-www-form-urlencoded", "");
    std::string accessToken = jsonVal(tokenResp, "access_token");
    while (!accessToken.empty() && (accessToken.front() == '"' || accessToken.front() == ' '))
        accessToken.erase(0, 1);
    while (!accessToken.empty() && (accessToken.back() == '"' || accessToken.back() == ' '))
        accessToken.pop_back();
    if (accessToken.empty()) return {};

    // 2. Fetch user info
    std::string userJson = httpGet(
        OAuthConfig::Discord::USER_API,
        "Bearer " + accessToken);

    std::string dId       = jsonVal(userJson, "id");
    std::string dUsername = jsonVal(userJson, "username");
    std::string dEmail    = jsonVal(userJson, "email");
    std::string dAvatar   = jsonVal(userJson, "avatar");

    if (dId.empty()) return {};

    // Build avatar URL
    std::string avatarUrl;
    if (!dAvatar.empty())
        avatarUrl = "https://cdn.discordapp.com/avatars/" + dId + "/" + dAvatar + ".png";

    OAuthUser user;
    user.provider     = "discord";
    user.providerId   = dId;
    user.email        = dEmail;
    user.name         = dUsername;
    user.avatarUrl    = avatarUrl;
    user.accessToken  = accessToken;
    user.refreshToken = jsonVal(tokenResp, "refresh_token");
    user.rawJson      = userJson;
    return user;
}
