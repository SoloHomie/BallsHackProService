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
#include <mutex>
#include <curl/curl.h>

class SmtpClient {
public:
    // Call once at startup (before any instance is created)
    static void init()  { curl_global_init(CURL_GLOBAL_DEFAULT); }
    // 注意: 不要调 curl_global_cleanup — 进程退出时 detached 邮件线程
    // 可能仍在 perform, cleanup 构成 use-after-free (2026-09-09)。

    // Send a MIME email via SMTP with STARTTLS.
    // Returns true on success. On failure, call lastError().
    // 线程安全: 多线程并发 sendMail 允许, lastError 由互斥锁保护。
    bool sendMail(const std::string &to, const std::string &subject,
                  const std::string &htmlBody,
                  std::function<void(const std::string &)> onLog = {});

    std::string lastError() const {
        std::lock_guard<std::mutex> lk(m_lastErrorMtx);
        return m_lastError;
    }

private:
    std::string buildPayload(const std::string &to,
                             const std::string &subject,
                             const std::string &htmlBody) const;
    static std::string rfc2822Date();

    std::string m_lastError;
    mutable std::mutex m_lastErrorMtx;  // 并发 sendMail 的写保护 (2026-09-09)
};
