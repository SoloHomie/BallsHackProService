#pragma once
#include <cstdint>

namespace ServerConfig {
    constexpr uint16_t PORT          = 8920;
    constexpr int      MAX_CONN      = 256;
    constexpr int      CODE_LENGTH   = 6;
    constexpr int      CODE_TTL_SEC  = 300;
    constexpr int      CODE_COOLDOWN = 60;
    constexpr int      MIN_PWD_LEN   = 6;
    constexpr int      MAX_PWD_LEN   = 20;
    constexpr int      BUF_SIZE      = 8192;
    constexpr const char* DOWNLOAD_URL = "https://api.solohomie.top/api/download";
}
