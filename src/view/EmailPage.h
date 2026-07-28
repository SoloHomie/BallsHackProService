#pragma once
// ── Verification code email renderer ─────────────────
// Loads the email HTML template from disk and substitutes
// verification code data via {{PLACEHOLDER}} markers.
#include <string>

namespace EmailPage {

/// Render the verification code email HTML body.
/// @param code    6-digit verification code
/// @param ttlMin  Validity in minutes
/// @param label   Purpose label (e.g. "注册", "登录", "重置密码")
/// @param desc    Description text for the email body
/// @param tmplPath  Filesystem path to the template file
/// @return Complete HTML string ready to send via SMTP.
std::string renderVerifyCode(const std::string &code, int ttlMin,
                             const std::string &label,
                             const std::string &desc,
                             const std::string &tmplPath = "html/VerifyCodeEmail.html");

} // namespace EmailPage
