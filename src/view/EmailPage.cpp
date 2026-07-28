// ── Verification code email renderer ─────────────────
#include "EmailPage.h"
#include "../core/config/Env.h"
#include "../common/StringUtils.h"
#include <fstream>
#include <sstream>

namespace EmailPage {

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

// ── Embedded fallback (minimal, no file dependency) ──
static const char *FALLBACK_TMPL = R"(<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1.0"></head>
<body style="margin:0;padding:0;background:#f5f7fb;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI','PingFang SC','Microsoft YaHei','Hiragino Sans GB',sans-serif;">
<table width="100%" cellpadding="0" cellspacing="0" style="background:#f5f7fb;">
<tr><td align="center" style="padding:48px 24px;">
<table width="480" cellpadding="0" cellspacing="0" style="max-width:480px;background:#ffffff;border-radius:20px;border:1px solid #eaedf2;box-shadow:0 8px 24px rgba(0,0,0,0.04);">
<tr><td style="padding:48px 40px 40px;">
<table cellpadding="0" cellspacing="0" style="margin:0 auto 20px;">
<tr><td align="center"><span style="font-size:48px;">&#x1f512;</span></td></tr></table>
<tr><td align="center" style="padding:0 0 4px;"><span style="color:#1a1a2e;font-size:26px;font-weight:700;letter-spacing:-0.5px;">BallsHackPro</span></td></tr>
<tr><td align="center" style="padding:0 0 8px;"><span style="color:#4f6ef7;font-size:20px;font-weight:600;letter-spacing:0.5px;">{{LABEL}}验证码</span></td></tr>
<tr><td align="center" style="padding:0 0 28px;"><span style="color:#6b7280;font-size:14px;line-height:1.6;">{{DESC}}</span></td></tr>
<tr><td align="center" style="padding:0 0 12px;">
<table cellpadding="0" cellspacing="0" style="background:#f0f4ff;border-radius:14px;padding:18px 40px;border:1px solid #e4ebfa;">
<tr><td align="center"><span style="color:#1a1a2e;font-size:48px;font-weight:700;font-family:'SF Mono','JetBrains Mono','Cascadia Code',monospace;letter-spacing:12px;">{{CODE}}</span></td></tr></table></td></tr>
<tr><td align="center" style="padding:0 0 36px;"><span style="color:#6b7280;font-size:15px;">验证码 <strong style="color:#1a1a2e;">{{TTL_MIN}}</strong> 分钟内有效，请尽快完成验证</span></td></tr>
<tr><td style="padding:0 0 28px;"><div style="border-top:1px solid #eef2f6;"></div></td></tr>
<tr><td style="padding:0 0 8px;">
<table cellpadding="0" cellspacing="0" style="background:#fafbff;border-radius:12px;border:1px solid #eef2f6;padding:16px 20px;width:100%;">
<tr><td style="font-size:14px;color:#374151;line-height:1.7;">
<span style="font-weight:700;color:#1a1a2e;display:block;margin-bottom:4px;"> 安全提醒</span>
· 请勿将此验证码<strong>转发</strong>或<strong>泄露</strong>给任何人，包括官方客服。<br>
· 我们<strong>绝不会</strong>以任何形式主动向您索要此验证码。<br>
· 如非本人操作，请忽略此邮件，并建议您<strong>立即检查账户安全设置</strong>。
</td></tr></table></td></tr>
</td></tr></table></td></tr></table></body></html>)";

std::string renderVerifyCode(const std::string &code, int ttlMin,
                             const std::string &label,
                             const std::string &desc,
                             const std::string &tmplPath) {
    // 1. Load template
    std::string t = readFile(tmplPath);
    if (t.empty())
        t = FALLBACK_TMPL;

    // 2. Substitute placeholders
    t = StringUtils::replace(t, "{{CODE}}",    code);
    t = StringUtils::replace(t, "{{TTL_MIN}}", std::to_string(ttlMin));
    t = StringUtils::replace(t, "{{LABEL}}",   label);
    t = StringUtils::replace(t, "{{DESC}}",    desc);

    return t;
}

} // namespace EmailPage
