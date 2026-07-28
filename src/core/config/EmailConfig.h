#pragma once
#include "Env.h"
#include <cstdint>
#include <string>

namespace EmailConfig {
    inline std::string SMTP_HOST()    { return Env::get("BH_SMTP_HOST", "127.0.0.1"); }
    inline int         SMTP_PORT()    { return std::stoi(Env::get("BH_SMTP_PORT", "1025")); }
    inline std::string AUTH_USER()    { return Env::get("BH_SMTP_USER", ""); }
    inline std::string AUTH_CODE()    { return Env::get("BH_SMTP_AUTH_CODE", ""); }
    constexpr const char *SENDER_NAME   = "BallsHackPro";
    constexpr int         EMAIL_COOLDOWN_SEC = 60;
    inline std::string SENDER_EMAIL() { return Env::get("BH_SMTP_SENDER", "noreply@ballshackpro.local"); }
}
