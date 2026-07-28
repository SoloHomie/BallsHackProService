#pragma once
// ── JSON 配置文件读取工具 ──────────────────────────
// 从 exe 同目录读取 .json 文件，返回解析后的内容
#include <string>
#include <fstream>
#include <sstream>
#include <optional>

namespace JsonConfig {

/// 读取整个文件为字符串
inline std::optional<std::string> readFile(const std::string &filename) {
    std::ifstream f(filename);
    if (!f) return {};
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

/// 从 JSON 字符串中提取指定 key 的值（简单解析，无需第三方库）
inline std::string getValue(const std::string &json, const std::string &key) {
    std::string pat = "\"" + key + "\":";
    auto pos = json.find(pat);
    if (pos == std::string::npos) return {};
    pos += pat.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (pos >= json.size()) return {};
    if (json[pos] == '"') {
        ++pos;
        auto end = json.find('"', pos);
        return (end == std::string::npos) ? "" : json.substr(pos, end - pos);
    }
    auto end = json.find_first_of(",}\n\r]", pos);
    return json.substr(pos, end - pos);
}

/// 从 JSON 文件读取指定 key 的值
inline std::string get(const std::string &filename, const std::string &key) {
    auto content = readFile(filename);
    return content ? getValue(*content, key) : "";
}

/// 从 JSON 文件读取布尔值
inline bool getBool(const std::string &filename, const std::string &key, bool fallback = false) {
    auto v = get(filename, key);
    if (v.empty()) return fallback;
    return v == "true" || v == "1";
}

/// 获取完整 JSON 字符串
inline std::string getRaw(const std::string &filename) {
    auto c = readFile(filename);
    return c ? *c : "";
}

} // namespace JsonConfig
