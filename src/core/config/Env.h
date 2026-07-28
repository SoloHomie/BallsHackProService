#pragma once
// ── Environment variable helpers ─────────────────────
// Reads configuration from environment variables with
// optional fallback defaults for development.
// Call Env::loadDotEnv() at startup to auto-load .env.
#include <cstdlib>
#include <string>
#include <fstream>
#include <sstream>
#include <cctype>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace Env {

inline std::string get(const char *name, const char *fallback = "") {
    const char *v = std::getenv(name);
    return v ? std::string(v) : std::string(fallback);
}

inline std::string required(const char *name) {
    const char *v = std::getenv(name);
    if (!v || !*v) return {};
    return std::string(v);
}

/// Resolve the directory containing the executable.
inline std::string exeDir() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        std::string path(buf, len);
        auto bs = path.rfind('\\');
        if (bs != std::string::npos) return path.substr(0, bs);
    }
    return ".";
#else
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        std::string path(buf, len);
        auto slash = path.rfind('/');
        if (slash != std::string::npos) return path.substr(0, slash);
    }
    return ".";
#endif
}

/// Load a .env file (KEY=VALUE per line) and set each as an env var.
/// Call once at the start of main().
/// Supports: # comments, empty lines, SET / export prefixes, quoted values.
/// Searches: 1) given path  2) exe dir / .env  3) exe dir / ../.env (VS debug).
inline void loadDotEnv(const std::string &path = ".env") {
    std::ifstream f(path);
    // Also try exe directory and parent directories
    if (!f) {
        std::string dir = exeDir();
        for (int up = 0; up <= 2 && !f; ++up) {
#ifdef _WIN32
            f.open(dir + "\\" + path);
#else
            f.open(dir + "/" + path);
#endif
            if (f) break;
            auto sep = dir.rfind(
#ifdef _WIN32
                '\\'
#else
                '/'
#endif
            );
            if (sep == std::string::npos) break;
            dir = dir.substr(0, sep);
        }
    }
    if (!f) return; // no .env file is OK — missing required() will catch it

    std::string line;
    while (std::getline(f, line)) {
        // Strip leading whitespace
        size_t s = 0;
        while (s < line.size() && std::isspace((unsigned char)line[s])) ++s;
        if (s >= line.size() || line[s] == '#') continue;
        line = line.substr(s);

        // Strip "SET " / "export " prefixes (case-insensitive)
        if (line.size() > 3 && (line[0] == 'S' || line[0] == 's') &&
                               (line[1] == 'E' || line[1] == 'e') &&
                               (line[2] == 'T' || line[2] == 't') &&
                                std::isspace((unsigned char)line[3]))
            line = line.substr(4);
        else if (line.size() > 6 && (line[0] == 'e' || line[0] == 'E') &&
                                    (line[1] == 'x' || line[1] == 'X') &&
                                    (line[2] == 'p' || line[2] == 'P') &&
                                    (line[3] == 'o' || line[3] == 'O') &&
                                    (line[4] == 'r' || line[4] == 'R') &&
                                    (line[5] == 't' || line[5] == 'T') &&
                                     std::isspace((unsigned char)line[6]))
            line = line.substr(7);

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        // Strip trailing whitespace from key
        while (!key.empty() && std::isspace((unsigned char)key.back())) key.pop_back();
        // Strip leading whitespace from value
        size_t vs = 0;
        while (vs < val.size() && std::isspace((unsigned char)val[vs])) ++vs;
        val = val.substr(vs);
        // Strip surrounding quotes
        if (val.size() >= 2 && (val.front() == '"' || val.front() == '\'') && val.front() == val.back()) {
            val = val.substr(1, val.size() - 2);
        }
        if (key.empty()) continue;

#ifdef _WIN32
        _putenv_s(key.c_str(), val.c_str());
#else
        setenv(key.c_str(), val.c_str(), 1);
#endif
    }
}

} // namespace Env
