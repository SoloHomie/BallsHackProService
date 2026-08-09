// ── OAuth callback page renderer ──────────────────────
#include "OAuthPage.h"
#include "../core/config/Env.h"
#include "../common/StringUtils.h"
#include <fstream>
#include <sstream>
#include <cctype>
#include <openssl/bio.h>
#include <openssl/evp.h>

namespace OAuthPage {

// ── Resolve asset path relative to exe directory ─────
static std::string resolvePath(const std::string &relative) {
    std::string dir = Env::exeDir();
    // Go up 2 levels from build dir (matches VS debug: x64/Debug → project root)
    for (int i = 0; i < 2; ++i) {
        auto pos = dir.rfind(
#ifdef _WIN32
            '\\'
#else
            '/'
#endif
        );
        if (pos != std::string::npos) dir = dir.substr(0, pos);
    }
#ifdef _WIN32
    return dir + "\\" + relative;
#else
    return dir + "/" + relative;
#endif
}

// ── Read entire file ─────────────────────────────────
static std::string readFile(const std::string &path) {
    std::string resolved = resolvePath(path);
    std::ifstream f(resolved, std::ios::binary);
    if (!f) {
        f.open(path, std::ios::binary);
    }
    if (!f) return {};
    std::ostringstream oss;
    oss << f.rdbuf();
    return oss.str();
}

// ── Capitalize first letter ──────────────────────────
static std::string capitalize(std::string s) {
    if (!s.empty()) s[0] = (char)std::toupper((unsigned char)s[0]);
    return s;
}

// ── Embedded fallback template ───────────────────────────
static const char *FALLBACK_TMPL = R"(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>{{TITLE}} - BallsHackPro</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{background:#0d1117;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px;color:#c9d1d9}
.card{background:#161b22;border:1px solid #30363d;border-radius:16px;padding:40px 32px 32px;max-width:400px;width:100%;text-align:center;box-shadow:0 8px 24px rgba(0,0,0,.6)}
.logo{width:64px;height:64px;margin:0 auto 18px}
.logo svg{width:100%;height:100%}
.brand{font-size:22px;font-weight:700;color:#f0f6fc;margin-bottom:16px}
.title{font-size:20px;font-weight:600;color:#f0f6fc;margin-bottom:4px}
.subtitle{font-size:14px;color:#8b949e;margin-bottom:18px}
.info-box{background:#0d1117;border-radius:10px;padding:14px 16px;margin-bottom:8px;text-align:left;border:1px solid #30363d}
.info-row{display:flex;justify-content:space-between;padding:5px 0;font-size:14px}
.info-row .label{color:#8b949e}
.info-row .value{color:#f0f6fc;font-weight:500}
.error-msg{color:#f85149;margin-bottom:16px}
</style>
</head>
<body>
<div class="card">
<div class="logo"><img src="{{LOGO_SRC}}" style="width:100%;height:100%" alt=""></div>
<div class="brand">BallsHack Pro</div>
<div id="statusArea"></div>
</div>
<div id="desktop-redirect" style="display:none">{{DESKTOP_REDIRECT}}</div>
<script>
(function(){
var postMsg={{POST_MESSAGE_DATA}};
var ok=postMsg.success;
if(window.opener)window.opener.postMessage(postMsg,window.location.origin);
var pn=postMsg.provider.charAt(0).toUpperCase()+postMsg.provider.slice(1);
if(ok){
document.getElementById('statusArea').innerHTML='<div class="title">授权成功</div><div class="subtitle">已通过 '+pn+' 登录</div>'+(postMsg.username?'<div class="info-box"><div class="info-row"><span class="label">用户名</span><span class="value">'+postMsg.username+'</span></div>'+(postMsg.email?'<div class="info-row"><span class="label">邮箱</span><span class="value">'+postMsg.email+'</span></div>':'')+'</div>':'')+'<div class="subtitle" style="margin-top:20px">请返回应用查看</div>';
}else{
document.getElementById('statusArea').innerHTML='<div class="title error-msg">授权失败</div><div class="subtitle">授权过程中出现异常，请重试</div><div class="subtitle" style="margin-top:20px">请返回应用查看</div>';
}
})();
</script>
</body></html>)";

// ── Provider themes ──────────────────────────────────
struct Theme { const char *bg,*card,*border,*green,*red,*muted,*text,*accent; };

static Theme themeFor(const std::string &provider) {
    if (provider == "discord")
        return {"#313338","#383a40","#4e5058","#57f287","#ed4245","#b5bac1","#f2f3f5","#00a8fc"};
    // default: GitHub-dark
    return {"#0d1117","#161b22","#30363d","#3fb950","#f85149","#8b949e","#e6edf3","#58a6ff"};
}

// ── Provider SVG icons ───────────────────────────────
static std::string iconSvg(const std::string &provider, const std::string &color) {
    if (provider == "discord")
        return "<svg viewBox=\"0 0 24 24\" width=\"14\" height=\"14\"><path fill=\"" + color + "\""
               " d=\"M20.317 4.37a19.791 19.791 0 00-4.885-1.515.074.074 0 00-.079.037c-.21.375-.444.864-.608 1.25a18.27 18.27 0 00-5.487 0 12.64 12.64 0 00-.617-1.25.077.077 0 00-.079-.037A19.736 19.736 0 003.677 4.37a.07.07 0 00-.032.027C.533 9.046-.32 13.58.099 18.057a.082.082 0 00.031.057 19.9 19.9 0 005.993 3.03.078.078 0 00.084-.028 14.09 14.09 0 001.226-1.994.076.076 0 00-.041-.106 13.107 13.107 0 01-1.872-.892.077.077 0 01-.008-.128 10.2 10.2 0 00.372-.292.074.074 0 01.077-.01c3.928 1.793 8.18 1.793 12.062 0a.074.074 0 01.078.01c.12.098.246.198.373.292a.077.077 0 01-.006.127 12.299 12.299 0 01-1.873.892.077.077 0 00-.041.107c.36.698.772 1.362 1.225 1.993a.076.076 0 00.084.028 19.839 19.839 0 006.002-3.03.077.077 0 00.032-.054c.5-5.177-.838-9.674-3.549-13.66a.061.061 0 00-.031-.03zM8.02 15.33c-1.183 0-2.157-1.085-2.157-2.419 0-1.333.956-2.419 2.157-2.419 1.21 0 2.176 1.096 2.157 2.42 0 1.333-.956 2.418-2.157 2.418zm7.975 0c-1.183 0-2.157-1.085-2.157-2.419 0-1.333.955-2.419 2.157-2.419 1.21 0 2.176 1.096 2.157 2.42 0 1.333-.946 2.418-2.157 2.418z\"/></svg>\n";
    // default: GitHub
    return "<svg viewBox=\"0 0 16 16\" width=\"14\" height=\"14\"><path fill=\"" + color + "\""
           " d=\"M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82a7.64 7.64 0 014 0c1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.013 8.013 0 0016 8c0-4.42-3.58-8-8-8z\"/></svg>\n";
}

// ── Build postMessage JSON payload (web / desktop) ────
static std::string postMessageJson(const OAuthCallbackData &d) {
    std::ostringstream js;
    js << "{success:" << (d.success ? "true" : "false")
       << ",access_token:\"" << d.token << "\""
       << ",refresh_token:\"" << d.refreshToken << "\""
       << ",email:\"" << d.email << "\""
       << ",username:\"" << d.username << "\""
       << ",uid:\"" << d.uid << "\""
       << ",provider:\"" << d.provider << "\"}";
    return js.str();
}

// ── Desktop callback: auto-submit POST form to localhost ──
// Tokens in POST body → not logged in browser history (vs query string)
std::string desktopRedirect(const OAuthCallbackData &d,
                                    const std::string &cbport) {
    if (!d.success || d.token.empty()) return "";
    if (!cbport.empty() && cbport != "0") {
        std::ostringstream html;
        html << "<form id=\"f\" method=\"POST\" action=\"http://localhost:" << cbport << "/callback\">"
             << "<input name=\"access_token\" value=\"" << StringUtils::htmlEscape(d.token) << "\">"
             << "<input name=\"refresh_token\" value=\"" << StringUtils::htmlEscape(d.refreshToken) << "\">"
             << "<input name=\"email\" value=\"" << StringUtils::htmlEscape(d.email) << "\">"
             << "<input name=\"username\" value=\"" << StringUtils::htmlEscape(d.username) << "\">"
             << "<input name=\"uid\" value=\"" << d.uid << "\">"
             << "<input name=\"provider\" value=\"" << d.provider << "\">"
             << "<input name=\"avatar\" value=\"" << StringUtils::htmlEscape(d.avatar) << "\">"
             << "<input name=\"mode\" value=\"" << (d.mode.empty() ? "login" : d.mode) << "\">"
             << "</form><script>setTimeout(function(){var f=document.getElementById('f');var d=new URLSearchParams();for(var i=0;i<f.elements.length;i++){var e=f.elements[i];if(e.name)d.append(e.name,e.value)}fetch(f.action,{method:'POST',body:d})},800)</script>";
        return html.str();
    }
    // Fallback: custom URI scheme (no browser history concern)
    std::ostringstream params;
    params << "access_token=" << StringUtils::urlEncode(d.token)
           << "&refresh_token=" << StringUtils::urlEncode(d.refreshToken)
           << "&email=" << StringUtils::urlEncode(d.email)
           << "&username=" << StringUtils::urlEncode(d.username)
           << "&uid=" << d.uid
           << "&provider=" << d.provider
           << "&avatar=" << StringUtils::urlEncode(d.avatar)
           << "&mode=" << (d.mode.empty() ? "login" : d.mode);
    return "ballshackpro://auth?" + params.str();
}

// ── Render ───────────────────────────────────────────
std::string render(const OAuthCallbackData &data,
                   const std::string &tmplPath,
                   const std::string &cbport) {
    // 1. Load template (file → fallback)
    std::string t = readFile(tmplPath);
    if (t.empty())
        t = FALLBACK_TMPL;

    // 2. Dynamic pieces (JS in template handles most rendering now)
    std::string postMsg = postMessageJson(data);
    std::string redirect = desktopRedirect(data, cbport);
    std::string title = data.success ? "授权成功" : "授权失败";

    // 3. Logo: load geluoluo.png as base64 data URI
    std::string logoSrc;
    {
        std::string logoPath = resolvePath("html/geluoluo.png");
        std::ifstream lf(logoPath, std::ios::binary | std::ios::ate);
        if (!lf) lf.open("html/geluoluo.png", std::ios::binary | std::ios::ate);
        if (lf) {
            auto sz = lf.tellg(); lf.seekg(0);
            std::string data(sz, '\0');
            lf.read(&data[0], sz);
            // Base64 encode via OpenSSL BIO
            BIO *b64 = BIO_new(BIO_f_base64()), *mem = BIO_new(BIO_s_mem());
            BIO_push(b64, mem);
            BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
            BIO_write(b64, data.data(), (int)data.size());
            BIO_flush(b64);
            char *enc = nullptr;
            long len = BIO_get_mem_data(mem, &enc);
            logoSrc = "data:image/png;base64," + std::string(enc, len);
            BIO_free_all(b64);
        }
    }

    // 4. Substitute placeholders
    t = StringUtils::replace(t, "{{LOGO_SRC}}",           logoSrc);
    t = StringUtils::replace(t, "{{TITLE}}",             title);
    t = StringUtils::replace(t, "{{POST_MESSAGE_DATA}}",  postMsg);
    t = StringUtils::replace(t, "{{DESKTOP_REDIRECT}}",   redirect);

    return t;
}

} // namespace OAuthPage
