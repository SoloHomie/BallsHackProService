#include "EmailService.h"
#include "SmtpClient.h"
#include "../view/EmailPage.h"
#include <sstream>
#include <openssl/bio.h>
#include <openssl/evp.h>

EmailService::EmailService() : m_smtp(std::make_unique<SmtpClient>()) {}
EmailService::~EmailService() = default;

// Encode UTF-8 string to RFC 2047 MIME encoded-word for email headers
static std::string mimeEncode(const std::string &utf8) {
    // Check if encoding is needed (only ASCII printable needs no encoding)
    bool needsEnc = false;
    for (unsigned char c : utf8) {
        if (c < 0x20 || c > 0x7E) { needsEnc = true; break; }
    }
    if (!needsEnc) return utf8;

    // Base64 encode
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *mem = BIO_new(BIO_s_mem());
    BIO_push(b64, mem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, utf8.data(), (int)utf8.size());
    BIO_flush(b64);
    char *encoded = nullptr;
    long len = BIO_get_mem_data(mem, &encoded);
    std::string result = "=?UTF-8?B?" + std::string(encoded, len) + "?=";
    BIO_free_all(b64);
    return result;
}

bool EmailService::sendVerifyCode(const std::string &to, const std::string &code,
                                   int ttlSeconds, const std::string &purpose) {
    int ttlMin = ttlSeconds / 60;
    const char *label = "Verification";
    if (purpose == "register") label = "注册";
    else if (purpose == "login")    label = "登录";
    else if (purpose == "reset")    label = "重置密码";

    const char *desc = "";
    if (purpose == "register") desc = "感谢注册 BallsHackPro，请使用以下验证码完成注册。";
    else if (purpose == "login")    desc = "有人正在尝试登录你的 BallsHackPro 账号。如果这是你本人操作，请使用以下验证码确认登录。";
    else if (purpose == "reset")    desc = "你正在重置 BallsHackPro 账号密码，请使用以下验证码完成操作。";

    std::string subject = mimeEncode(std::string("BallsHackPro ") + label + "验证码");
    std::string html = buildHtml(code, ttlMin, label, desc);

    {
        std::lock_guard<std::mutex> lk(m_lastErrorMtx);
        m_lastError.clear();
    }
    return m_smtp->sendMail(to, subject, html,
        [this](const std::string &msg) {
            std::lock_guard<std::mutex> lk(m_lastErrorMtx);
            m_lastError = msg;
        });
}

std::string EmailService::buildHtml(const std::string& code, int ttlMin,
                                     const char* label, const char* desc) const {
    return EmailPage::renderVerifyCode(code, ttlMin, label, desc);
}
