#pragma once
#include "Env.h"
#include <cstdint>

namespace DbConfig {
    constexpr const char* HOST     = "127.0.0.1";
    constexpr uint16_t   PORT     = 3306;
    constexpr const char* DATABASE = "balls_hack_pro";
    constexpr int        POOL_SIZE          = 10;
    constexpr int        ACQUIRE_TIMEOUT_MS = 5000;

    inline std::string USER()     { return Env::get("BH_DB_USER", "bhpro_app"); }
    inline std::string PASSWORD() { return Env::get("BH_DB_PASSWORD", "bhpro_pass"); }
}
