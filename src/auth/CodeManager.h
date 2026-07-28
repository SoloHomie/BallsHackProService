#pragma once
#include <string>

namespace CodePurpose {
    constexpr const char *REGISTER   = "register";
    constexpr const char *LOGIN      = "login";
    constexpr const char *RESET      = "reset";
    constexpr const char *BOT_VERIFY = "bot_verify";
}

class CodeManager {
public:
    CodeManager() = default;

    std::string generate(const std::string &email, const std::string &purpose);
    bool validate(const std::string &email, const std::string &code,
                  const std::string &purpose);
    bool canSend(const std::string &email, const std::string &purpose) const;
    int  cooldownRemaining(const std::string &email, const std::string &purpose) const;
    void purgeExpired(); // no-op with Redis — keys auto-expire

private:
    std::string randomCode();

    static std::string codeKey(const std::string &email, const std::string &purpose) {
        return "code:" + email + "|" + purpose;
    }
    static std::string cdKey(const std::string &email, const std::string &purpose) {
        return "cooldown:" + email + "|" + purpose;
    }
    static std::string usedKey(const std::string &code) {
        return "used:" + code;
    }
};
