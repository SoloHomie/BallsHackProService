#pragma once
#include <string>
#include <memory>

class SmtpClient;

class EmailService {
public:
    EmailService();
    ~EmailService();

    // Send verification code email. purpose: register | login | reset
    bool sendVerifyCode(const std::string &to, const std::string &code,
                        int ttlSeconds, const std::string &purpose);

    // Get last error message
    std::string lastError() const { return m_lastError; }

private:
    std::string buildHtml(const std::string &code, int ttlMinutes,
                          const char *label, const char *desc) const;

    std::unique_ptr<SmtpClient> m_smtp;
    std::string m_lastError;
};
