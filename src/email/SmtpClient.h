#pragma once
// ── libcurl-based SMTP client (大厂标配) ──────────────
// libcurl is the industry standard — used by Google, Meta, Apple,
// Netflix, and virtually every major C/C++ codebase.
//
// Dependencies installed via vcpkg:
//   vcpkg install curl:x64-windows
//   vcpkg integrate install

#include <string>
#include <functional>
#include <curl/curl.h>

class SmtpClient {
public:
    // Call once at startup (before any instance is created)
    static void init()  { curl_global_init(CURL_GLOBAL_DEFAULT); }
    // Call once at shutdown
    static void cleanup() { curl_global_cleanup(); }

    // Send a MIME email via SMTP with STARTTLS.
    // Returns true on success. On failure, call lastError().
    bool sendMail(const std::string &to, const std::string &subject,
                  const std::string &htmlBody,
                  std::function<void(const std::string &)> onLog = {});

    std::string lastError() const { return m_lastError; }

private:
    std::string buildPayload(const std::string &to,
                             const std::string &subject,
                             const std::string &htmlBody) const;
    static std::string rfc2822Date();

    std::string m_lastError;
};
