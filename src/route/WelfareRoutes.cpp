#include "WelfareRoutes.h"
#include "../core/server/HttpServer.h"
#include "../auth/AuthService.h"
#include "../common/StringUtils.h"
#include "../core/cache/SubCache.h"
#include "../welfare/WelfareStore.h"
#include <string>

// 2026-09-06 重构: 福利 SQL 全部收敛到 WelfareStore,
// 本文件只做 鉴权 → 调 Store → 组 JSON。
void registerWelfareRoutes(HttpServer &server, AuthService &auth, DbPool &pool) {
    static WelfareStore store(pool);   // static: 与 server 同生命周期, lambda 引用安全

    // ── 可领取列表 ──
    server.route("GET", "/api/welfare/available",
        [&](const std::string &, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
            uint64_t uid = std::stoull(tp->sub);

            auto events = store.listAvailable(uid, tp->email);
            std::string json = "{\"success\":true,\"events\":[";
            bool first = true;
            for (const auto &e : events) {
                if (!first) json += ","; first = false;
                json += "{\"id\":" + std::to_string(e.id)
                     + ",\"name\":\"" + AuthService::jsonEscape(e.name) + "\""
                     + ",\"desc\":\"" + AuthService::jsonEscape(e.desc) + "\""
                     + ",\"type\":\"" + AuthService::jsonEscape(e.type) + "\""
                     + ",\"amount\":" + std::to_string(e.amount)
                     + ",\"starts\":\"" + e.starts + "\""
                     + ",\"expires\":\"" + e.expires + "\""
                     + ",\"claimed\":" + std::to_string(e.claimed)
                     + "}";
            }
            json += "]}";
            return json;
        });

    // ── 领取福利 ──
    server.route("POST", "/api/welfare/claim",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
            uint64_t uid = std::stoull(tp->sub);
            std::string eidStr = StringUtils::jsonVal(body, "event_id");
            if (eidStr.empty()) return AuthService::jsonErr("FIELDS_REQUIRED", "请选择福利");
            uint64_t eid = std::stoull(eidStr);

            auto res = store.claim(uid, eid, tp->email);
            if (!res.ok) return AuthService::jsonErr(res.errcode, res.msg);

            // 同步订阅缓存 (唯一实现 SubCache::write)
            auto u = auth.userStore().findById(uid);
            SubCache::write(uid, u ? u->subscription_expires : "");

            return AuthService::jsonOk("领取成功！获得 " + std::to_string(res.days) + " 天 Pro 会员~",
                                       "days", std::to_string(res.days));
        });
}
