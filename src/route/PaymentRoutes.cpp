#include "PaymentRoutes.h"
#include "../core/server/HttpServer.h"
#include "../auth/AuthService.h"
#include "../payment/VPayService.h"
#include "../common/StringUtils.h"
#include "../core/config/Env.h"
#include "../core/database/DbPool.h"
#include "../core/cache/RedisClient.h"
#include <mysql/mysql.h>
#include <sstream>

void registerPaymentRoutes(HttpServer &server, AuthService &auth, DbPool &pool) {

    static VPayService payService(
        pool,
        Env::get("PAY_API_URL", ""),
        Env::get("PAY_APP_ID", ""),
        "rsa_private.pem",
        Env::get("PAY_APP_KEY", "JjWvv0eucsHVWMUZzMdgJCuc1aMsgdAc"),
        Env::get("PAY_NOTIFY_URL", "")
    );

    // ── 套餐列表 ──
    server.route("GET", "/api/payment/plans",
        [&](const std::string &, const std::string &, const std::string &) {
            std::string j = "{\"success\":true,\"plans\":["; bool f=true;
            for (auto &p : VPayService::plans()) {
                if (!f) j+=","; f=false;
                j += "{\"product_id\":\""+AuthService::jsonEscape(p.productId)+"\""
                   + ",\"name\":\""+AuthService::jsonEscape(p.name)+"\""
                   + ",\"amount\":"+std::to_string(p.amount)
                   + ",\"days\":"+std::to_string(p.days)+"}";
            }
            j += "]}"; return j;
        });

    // ── 创建订单 ──
    server.route("POST", "/api/payment/order",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
            uint64_t uid = std::stoull(tp->sub);
            std::string pid = StringUtils::jsonVal(body, "product_id");
            if (pid.empty()) return AuthService::jsonErr("FIELDS_REQUIRED", "请选择套餐");
            auto [ok, result] = payService.createOrder(uid, pid);
            if (!ok) return AuthService::jsonErr("ORDER_FAILED", result);
            return "{\"success\":true,"+result.substr(1);
        });

    // ── 回调（GET 请求，MD5 验签）──
    server.route("GET", "/api/payment/callback",
        [&](const std::string &, const std::string &query, const std::string &) {
            bool ok = payService.handleCallback(query);
            return ok ? std::string("success") : std::string("fail");
        });

    // ── 查询订单状态（含过期检测）──────
    server.route("GET", "/api/payment/order/status",
        [&](const std::string &, const std::string &query, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
            std::string oid = StringUtils::queryVal(query, "order_id");
            if (oid.empty()) return AuthService::jsonErr("ID_REQUIRED", "请提供订单ID");
            // 先查 Redis — 过期则回退查 DB 真实状态, 不让用户承担后果
            bool expired = true;
            try { auto v = RedisClient::instance().get("pay:order:" + oid); expired = !v; } catch(...) {}
            if (expired) {
                auto o = payService.queryOrder(std::stoull(oid));
                if (!o) return std::string("{\"success\":true,\"status\":\"expired\"}");
                if (o->status == "paid")
                    return std::string("{\"success\":true,\"status\":\"paid\"}");
                return std::string("{\"success\":true,\"status\":\"expired\"}");
            }
            auto o = payService.queryOrder(std::stoull(oid));
            if (!o) return AuthService::jsonErr("NOT_FOUND", "订单不存在");
            std::ostringstream j;
            j<<"{\"success\":true,\"status\":\""<<AuthService::jsonEscape(o->status)<<"\""
             <<",\"id\":"<<o->orderId<<",\"amount\":"<<o->amount
             <<",\"duration_days\":"<<o->durationDays<<"}";
            return j.str();
        });
}
