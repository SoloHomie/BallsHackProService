// ── libcurl SMTP client ──────────────────────────────
// Uses libcurl's production-grade SMTP module.
// STARTTLS, AUTH LOGIN, MIME formatting — all handled by curl.

#include "SmtpClient.h"
#include "../core/config/EmailConfig.h"
#include "../core/config/Env.h"
#include "../common/Logger.h"

#include <curl/curl.h>
#include <ctime>
#include <cstring>
#include <iostream>
#include <sstream>
#include <iomanip>

// ── Upload state for CURLOPT_READFUNCTION ─────────────
struct UploadContext {
    const std::string &data;
    size_t offset = 0;
};

static size_t readCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *ctx = static_cast<UploadContext *>(userdata);
    size_t room  = size * nmemb;
    size_t avail = ctx->data.size() - ctx->offset;
    size_t n     = avail < room ? avail : room;
    if (n == 0) return 0;
    std::memcpy(ptr, ctx->data.data() + ctx->offset, n);
    ctx->offset += n;
    return n;
}

// ── Debug trace → onLog ───────────────────────────────
static int debugCallback(CURL *, curl_infotype type,
                         char *data, size_t size, void *userptr) {
    if (type != CURLINFO_TEXT) return 0;
    auto *fn = static_cast<std::function<void(const std::string &)> *>(userptr);
    if (!fn || !(*fn)) return 0;
    std::string msg(data, size);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
        msg.pop_back();
    (*fn)(msg);
    return 0;
}

// ── Global init moved to SmtpClient::init() / cleanup() (called from main) ──

// ── RFC 2822 date ─────────────────────────────────────
std::string SmtpClient::rfc2822Date() {
    auto now = std::time(nullptr);
    struct tm t;
#ifdef _WIN32
    gmtime_s(&t, &now);
#else
    gmtime_r(&now, &t);
#endif
    static const char *days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char *mons[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                  "Jul","Aug","Sep","Oct","Nov","Dec"};
    std::ostringstream oss;
    oss << days[t.tm_wday] << ", "
        << std::setw(2) << std::setfill('0') << t.tm_mday << ' '
        << mons[t.tm_mon] << ' ' << (1900 + t.tm_year) << ' '
        << std::setw(2) << std::setfill('0') << t.tm_hour << ':'
        << std::setw(2) << std::setfill('0') << t.tm_min  << ':'
        << std::setw(2) << std::setfill('0') << t.tm_sec
        << " +0000";
    return oss.str();
}

// ── Build MIME payload ────────────────────────────────
std::string SmtpClient::buildPayload(const std::string &to,
                                     const std::string &subject,
                                     const std::string &htmlBody) const {
    std::ostringstream m;
    m << "Date: " << rfc2822Date() << "\r\n"
      << "To: <" << to << ">\r\n"
      << "From: " << EmailConfig::SENDER_NAME
      << " <" << EmailConfig::SENDER_EMAIL() << ">\r\n"
      << "Subject: " << subject << "\r\n"
      << "MIME-Version: 1.0\r\n"
      << "Content-Type: text/html; charset=\"UTF-8\"\r\n"
      << "Content-Transfer-Encoding: 8bit\r\n"
      << "\r\n"
      << htmlBody;
    return m.str();
}

// ── Send mail ─────────────────────────────────────────
bool SmtpClient::sendMail(const std::string &to,
                          const std::string &subject,
                          const std::string &htmlBody,
                          std::function<void(const std::string &)> onLog) {
    m_lastError.clear();
    std::function<void(const std::string &)> logFn = onLog;

    CURL *curl = curl_easy_init();
    if (!curl) {
        m_lastError = "curl_easy_init() failed";
        return false;
    }

    // Build MIME payload
    std::string payload = buildPayload(to, subject, htmlBody);
    UploadContext ctx{payload, 0};

    // Recipient list
    struct curl_slist *recipients = nullptr;
    recipients = curl_slist_append(recipients, ("<" + to + ">").c_str());

    // SMTP URL: smtp:// with STARTTLS on port 587
    std::string url = std::string("smtp://") +
                      EmailConfig::SMTP_HOST() + ":" +
                      std::to_string(EmailConfig::SMTP_PORT());

    // ── Configure curl (store strings locally; passing std::string
    //     directly to curl C variadic function is undefined behavior)
    std::string senderEmail = EmailConfig::SENDER_EMAIL();
    std::string authUser    = EmailConfig::AUTH_USER();
    std::string authCode    = EmailConfig::AUTH_CODE();
    std::string mailFrom    = "<" + senderEmail + ">";

    curl_easy_setopt(curl, CURLOPT_URL,              url.c_str());
    // SMTP 海外直连，不走代理（代理通常不支持 SMTP CONNECT 隧道）
    std::string proxy = Env::get("BH_SMTP_PROXY");
    if (!proxy.empty()) {
        curl_easy_setopt(curl, CURLOPT_PROXY,            proxy.c_str());
        curl_easy_setopt(curl, CURLOPT_PROXYTYPE,        CURLPROXY_HTTP);
        curl_easy_setopt(curl, CURLOPT_HTTPPROXYTUNNEL,  1L);
    }
    curl_easy_setopt(curl, CURLOPT_USERNAME,         authUser.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD,         authCode.c_str());
    curl_easy_setopt(curl, CURLOPT_USE_SSL,          CURLUSESSL_ALL);
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM,        mailFrom.c_str());
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT,        recipients);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION,     readCallback);
    curl_easy_setopt(curl, CURLOPT_READDATA,         &ctx);
    curl_easy_setopt(curl, CURLOPT_UPLOAD,           1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,   30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,          60L);

    // Debug tracing
    if (logFn) {
        curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, debugCallback);
        curl_easy_setopt(curl, CURLOPT_DEBUGDATA,     &logFn);
        curl_easy_setopt(curl, CURLOPT_VERBOSE,        1L);
    }

    // ── Send ───────────────────────────────────────
    CURLcode res = curl_easy_perform(curl);

    curl_slist_free_all(recipients);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        m_lastError = std::string("SMTP: ") + curl_easy_strerror(res);
        Logger::err("Email", "SMTP send failed to " + to + ": " + m_lastError);
        if (logFn) logFn(m_lastError);
        return false;
    }

    if (logFn) logFn("Mail sent OK → " + to);
    return true;
}
