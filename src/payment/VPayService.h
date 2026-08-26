#pragma once
#include <string>
#include <cstdint>
#include <optional>
#include <vector>

class DbPool;
struct PayOrder { uint64_t orderId, userId, amount; uint32_t durationDays; std::string status, tradeNo; };
struct PricePlan { std::string productId, name; uint64_t amount; uint32_t days; };

// jkou 易支付 V2 (pay.jkou.cn) 接口封装 — 仅 RSA 签名 (SHA256WithRSA)
// 下单: POST {apiUrl}/api/pay/create  商户私钥签名, method=jump 返回支付跳转链接
// 回调: GET notify_url, 全部字段参与 RSA 验签 (平台公钥) + trade_status/pid/金额/时间戳校验
// 查单: POST {apiUrl}/api/pay/query  (回调丢失时主动补账)
class VPayService {
public:
    VPayService(DbPool &pool, const std::string &apiUrl, const std::string &appId,
                const std::string &md5Key, const std::string &notifyUrl,
                const std::string &returnUrl);
    std::pair<bool, std::string> createOrder(uint64_t userId, const std::string &productId,
                                             const std::string &clientIp,
                                             const std::string &payType = "alipay");
    bool handleCallback(const std::string &query);
    bool syncOrderStatus(uint64_t orderId);   // 平台查单 + 已付补账 (幂等)
    std::optional<PayOrder> queryOrder(uint64_t orderId);
    bool refundOrder(uint64_t orderId, const std::string &moneyYuan, std::string &err);  // ikunpay refund
    // 对账扫描: 返回 cutoffId (雪花ID时间下界) 之后创建的未支付订单ID
    std::vector<uint64_t> pendingOrdersSince(uint64_t cutoffId, int limit);
    static const std::vector<PricePlan>& plans();
private:
    std::string readFile(const std::string &path) const;
    std::string buildSignStr(const std::vector<std::pair<std::string,std::string>> &params) const;
    std::string rsaSignSha256(const std::string &data) const;
    bool rsaVerifySha256(const std::string &data, const std::string &b64sig) const;
    bool applyPayment(uint64_t orderId, const std::string &tradeNo);  // 幂等补账
    uint32_t daysForProduct(const std::string &pid) const;
    DbPool &m_pool;
    std::string m_apiUrl, m_appId, m_md5Key, m_notifyUrl, m_returnUrl;
};
