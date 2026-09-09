#pragma once
#include <string>
#include <memory>
#include <mutex>

class SmtpClient;

class EmailService {
public:
    EmailService();
    ~EmailService();

    // Send verification code email. purpose: register | login | reset
    // 线程安全: 多 detached 线程并发调用, lastError 由互斥锁保护 (2026-09-09)
    bool sendVerifyCode(const std::string &to, const std::string &code,
                        int ttlSeconds, const std::string &purpose);

    // Get last error message
    std::string lastError() const {
        std::lock_guard<std::mutex> lk(m_lastErrorMtx);
        return m_lastError;
    }

private:
    std::string buildHtml(const std::string &code, int ttlMinutes,
                          const char *label, const char *desc) const;

    std::unique_ptr<SmtpClient> m_smtp;
    std::string m_lastError;
    mutable std::mutex m_lastErrorMtx;
};
