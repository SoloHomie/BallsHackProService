#include "StringUtils.h"
#include <nlohmann/json.hpp>

namespace StringUtils {

// ── jsonVal — nlohmann 解析版 (2026-09-06 重构) ───────────────────
// 替换原手写子串匹配: 原实现遇到转义引号会截断字符串、对嵌套/空白变体
// 不可靠。语义保持: 顶层键取值; 字符串返回原文(转义已还原),
// 数字/布尔/对象/数组返回紧凑 JSON 文本; 键不存在/JSON 非法 → 空串。
std::string jsonVal(const std::string &json, const std::string &key) {
    if (json.empty() || key.empty()) return {};
    try {
        nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
        if (!j.is_object()) return {};
        auto it = j.find(key);
        if (it == j.end()) return {};
        if (it->is_string()) return it->get<std::string>();
        return it->dump();   // number/bool/object/array → 文本形式
    } catch (...) {
        return {};
    }
}

} // namespace StringUtils
