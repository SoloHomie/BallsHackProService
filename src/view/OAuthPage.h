#pragma once
// ── OAuth callback page renderer ──────────────────────
// Loads an HTML template from disk and substitutes
// OAuth result data using {{PLACEHOLDER}} markers.
// Falls back to an embedded template if the file is
// unavailable (e.g. in deployment with missing assets).
#include <string>

namespace OAuthPage {

/// Data passed to the OAuth callback page renderer.
struct OAuthCallbackData {
    bool        success  = false;
    std::string provider;
    std::string email;
    std::string username;
    std::string avatar;
    std::string token;         // access_token (JWT)
    std::string refreshToken;  // refresh_token
    std::string uid;           // user id
    std::string mode;          // "bind" = 关联模式, "" = 登录模式
};

/// Render the OAuth callback HTML page.
/// @param cbport  If >0, redirect to http://localhost:<cbport>/callback?...
///                instead of ballshackpro:// (avoids Smart App Control).
std::string render(const OAuthCallbackData &data,
                   const std::string &tmplPath = "html/OAuthCallback.html",
                   const std::string &cbport = "");

/// 桌面端重定向：最小化 HTML，即时 POST 到本地 OAuthServer，无可见内容
std::string desktopRedirect(const OAuthCallbackData &data,
                            const std::string &cbport = "");

} // namespace OAuthPage
