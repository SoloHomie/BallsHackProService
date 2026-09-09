#include "RedeemRoutes.h"
#include "RouteGuard.h"
#include "../core/server/HttpServer.h"
#include "../auth/AuthService.h"
#include "../common/StringUtils.h"
#include "../core/cache/SubCache.h"
#include "../core/cache/RedisClient.h"
#include "../redeem/RedeemStore.h"
#include <string>

// 兑换码兑换 Pro 时长。
// 2026-09-09 重构: 码规范化+事务全部收敛到 RedeemStore (src/redeem/),
// 本文件只做 鉴权 → 参数解析 → Redis 限流 → 调 Store → 组 JSON。
void registerRedeemRoutes(HttpServer &server, AuthService &auth, DbPool &pool) {
    static RedeemStore store(pool);   // static: 与 server 同生命周期, lambda 引用安全

    server.route("POST", "/api/welfare/redeem",
        [&](const std::string &body, const std::string &, const std::string &) {
            const auto &tp = route::currentUser();
            uint64_t uid = std::stoull(tp.sub);

            // ── Redis 限流: 10次/10分钟 (协议层防暴力猜码) ──
            try {
                auto &r = RedisClient::instance();
                auto cnt = r.get("rl:redeem:" + std::to_string(uid));
                int c = cnt ? std::stoi(*cnt) : 0;
                if (c >= 10)
                    return AuthService::jsonErr("TOO_MANY_ATTEMPTS", "尝试次数过多，请10分钟后再试");
                r.incr("rl:redeem:" + std::to_string(uid));
                r.expire("rl:redeem:" + std::to_string(uid), 600);
            } catch (...) {}

            auto res = store.redeem(uid, StringUtils::jsonVal(body, "code"));
            if (!res.ok) return AuthService::jsonErr(res.errcode, res.msg);

            // 同步订阅缓存 (唯一实现 SubCache::write)
            if (!res.newExpires.empty())
                SubCache::write(uid, res.newExpires);

            std::string msg = "兑换成功！获得 " + std::to_string(res.days) + " 天 Pro 会员~";
            return "{\"success\":true,\"message\":\"" + AuthService::jsonEscape(msg)
                 + "\",\"days\":" + std::to_string(res.days)
                 + ",\"subscribed\":true"
                 + ",\"subscription_expires\":\"" + AuthService::jsonEscape(res.newExpires) + "\"}";
        }, route::requireAuth(auth));
}
