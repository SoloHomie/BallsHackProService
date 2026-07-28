#pragma once
#include <string>
#include <cstdint>
#include <optional>
#include <vector>
#include <unordered_map>

class DbPool;
struct PayOrder { uint64_t orderId, userId, amount; uint32_t durationDays; std::string status, tradeNo; };
struct PricePlan { std::string productId, name; uint64_t amount; uint32_t days; };

class VPayService {
public:
    VPayService(DbPool &pool, const std::string &apiUrl, const std::string &appId,
                const std::string &keyPath, const std::string &md5Key, const std::string &notifyUrl);
    std::pair<bool, std::string> createOrder(uint64_t userId, const std::string &productId);
    bool handleCallback(const std::string &query);
    std::optional<PayOrder> queryOrder(uint64_t orderId);
    static const std::vector<PricePlan>& plans();
private:
    std::string readFile(const std::string &path) const;
    std::string rsaSign(const std::string &data) const;
    std::string buildSignStr(const std::vector<std::pair<std::string,std::string>> &params) const;
    uint32_t daysForProduct(const std::string &pid) const;
    DbPool &m_pool; std::string m_apiUrl, m_appId, m_rsaKey, m_md5Key, m_notifyUrl;
};
