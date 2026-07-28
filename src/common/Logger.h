#pragma once
// ── Structured logging utility (header-only) ──────────
// Usage:
//   Logger::info("Module", "message");
//   Logger::warn("Module", "warning");
//   Logger::err("Module",  "error");
//   Logger::dbg("Module",  "debug detail");
//
// Note: enum values are prefixed to avoid collisions with
// Windows SDK macros (DEBUG, ERROR) and <cmath> (log).
#include <iostream>
#include <string>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <ctime>

namespace Logger {

enum Level { L_DEBUG, L_INFO, L_WARN, L_ERROR };

inline const char* levelStr(Level l) {
    switch (l) {
        case L_DEBUG: return "DEBUG";
        case L_INFO:  return "INFO ";
        case L_WARN:  return "WARN ";
        case L_ERROR: return "ERROR";
    }
    return "????";
}

inline void write(Level level, const std::string& module, const std::string& msg) {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
        << '.' << std::setw(3) << std::setfill('0') << ms.count()
        << " [" << levelStr(level) << "] "
        << "[" << module << "] " << msg;

    if (level >= L_WARN)
        std::cerr << oss.str() << std::endl;
    else
        std::cout << oss.str() << std::endl;
}

inline void info(const std::string& m, const std::string& msg) { write(L_INFO,  m, msg); }
inline void warn(const std::string& m, const std::string& msg) { write(L_WARN,  m, msg); }
inline void err(const std::string&  m, const std::string& msg) { write(L_ERROR, m, msg); }
inline void dbg(const std::string&  m, const std::string& msg) { write(L_DEBUG, m, msg); }

} // namespace Logger
