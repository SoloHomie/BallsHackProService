#include "PeerRoutes.h"
#include "RouteGuard.h"
#include "../core/server/HttpServer.h"
#include "../auth/AuthService.h"
#include "../common/StringUtils.h"
#include "../peer/PeerStore.h"
#include <string>
#include <sstream>

// ═══════════════════════════════════════════════════════════════
//  P2P 信令 + Relay 中转 — 协议层 (2026-09-09 重构)
//  Redis 存储全部收敛到 src/peer/PeerStore, 本文件只做:
//  参数解析 → 调 Store → 组 JSON。
//  流程: 控制方 create → 被控方 join → 双方 peers/push/pull。
// ═══════════════════════════════════════════════════════════════

// ── 提取客户端真实 IP (Cloudflare → Caddy → App) ──
static std::string extractClientIp(const std::string &headers) {
    auto getHdr = [&](const std::string &name) -> std::string {
        // 精确匹配 "Name: value\r\n"
        auto pos = headers.find(name + ": ");
        if (pos == std::string::npos) {
            // 尝试小写
            std::string lower = name;
            for (auto &c : lower) c = (c >= 'A' && c <= 'Z') ? c + 32 : c;
            pos = headers.find(lower + ": ");
            if (pos == std::string::npos) return {};
        }
        pos = headers.find(": ", pos) + 2;
        auto end = headers.find("\r\n", pos);
        if (end == std::string::npos) end = headers.size();
        return headers.substr(pos, end - pos);
    };

    std::string ip;
    // 优先级: CF-Connecting-IP (Cloudflare) > X-Real-IP (Caddy) > X-Forwarded-For
    ip = getHdr("CF-Connecting-IP");
    if (ip.empty()) ip = getHdr("X-Real-IP");
    if (ip.empty()) {
        std::string xff = getHdr("X-Forwarded-For");
        if (!xff.empty()) {
            auto comma = xff.find(',');
            ip = (comma != std::string::npos) ? xff.substr(0, comma) : xff;
        }
    }
    // 去空格
    while (!ip.empty() && ip.front() == ' ') ip.erase(0, 1);
    while (!ip.empty() && ip.back() == ' ') ip.pop_back();
    if (ip.empty()) ip = "127.0.0.1";
    return ip;
}

// ── Peer → JSON ──
static std::string peerJson(const PeerStore::Peer &p) {
    std::ostringstream s;
    s << "{\"ip\":\"" << StringUtils::jsonEscape(p.ip) << "\",\"port\":" << p.port << "}";
    return s.str();
}

// ── 解析 bool (兼容 "true"/"1"/true) ──
static bool parseBool(const std::string &s) {
    return s == "true" || s == "1" || s == "TRUE" || s == "True";
}

void registerPeerRoutes(HttpServer &server, AuthService &auth, DbPool &) {
    static PeerStore store;   // Redis 存储层, 无需 DbPool

    // ── 控制方创建房间 {port, uid} → 房码 + slot0 ──
    server.route("POST", "/api/peer/create",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            const auto &tp = route::currentUser();

            std::string portStr = StringUtils::jsonVal(body, "port");
            std::string uid = StringUtils::jsonVal(body, "uid");
            int port = 0;
            try { port = std::stoi(portStr); }
            catch (...) { return AuthService::jsonErr("FIELDS_REQUIRED", "需要有效 port"); }
            if (port <= 0 || port > 65535)
                return AuthService::jsonErr("FIELDS_REQUIRED", "port 范围 1-65535");
            if (uid.empty() || uid.size() > 64)
                return AuthService::jsonErr("FIELDS_REQUIRED", "需要有效 uid");

            std::string addr = extractClientIp(headers) + ":" + std::to_string(port);
            auto res = store.createRoom(tp.email, addr, uid);
            if (!res.ok) return AuthService::jsonErr(res.errcode, res.msg);

            std::ostringstream resp;
            resp << "{\"success\":true"
                 << ",\"room_code\":\"" << res.roomCode << "\""
                 << ",\"member_count\":1"
                 << ",\"member_max\":2"
                 << ",\"role\":\"controller\""
                 << "}";
            return resp.str();
        }, route::requireAuth(auth));

    // ── 被控制方加入 {room_code, port, uid} → slot1 + 对方地址 ──
    server.route("POST", "/api/peer/join",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            std::string code = StringUtils::jsonVal(body, "room_code");
            std::string portStr = StringUtils::jsonVal(body, "port");
            std::string uid = StringUtils::jsonVal(body, "uid");
            int port = 0;
            try { port = std::stoi(portStr); }
            catch (...) { return AuthService::jsonErr("FIELDS_REQUIRED", "需要有效 port"); }
            if (code.empty() || port <= 0 || port > 65535)
                return AuthService::jsonErr("FIELDS_REQUIRED", "需要 room_code 和有效 port");
            if (uid.empty() || uid.size() > 64)
                return AuthService::jsonErr("FIELDS_REQUIRED", "需要有效 uid");

            std::string addr = extractClientIp(headers) + ":" + std::to_string(port);
            auto res = store.joinRoom(code, addr, uid);
            if (!res.ok) return AuthService::jsonErr(res.errcode, res.msg);

            std::ostringstream resp;
            if (res.reconnect) {
                resp << "{\"success\":true"
                     << ",\"reconnect\":true"
                     << ",\"peer\":" << peerJson(*res.peer)
                     << ",\"member_count\":2"
                     << ",\"member_max\":2"
                     << "}";
                return resp.str();
            }
            resp << "{\"success\":true"
                 << ",\"peer\":" << peerJson(*res.peer)
                 << ",\"member_count\":2"
                 << ",\"member_max\":2"
                 << ",\"role\":\"controlled\""
                 << "}";
            return resp.str();
        }, route::requireAuth(auth));

    // ── 房间状态 ?room=XXX ──
    server.route("GET", "/api/peer/status",
        [&](const std::string &, const std::string &query, const std::string &) {
            std::string code = StringUtils::queryVal(query, "room");
            if (code.empty())
                return AuthService::jsonErr("FIELDS_REQUIRED", "缺少 room 参数");

            auto res = store.roomStatus(code);
            std::ostringstream resp;
            resp << "{\"success\":true"
                 << ",\"room_code\":\"" << code << "\""
                 << ",\"member_count\":" << res.memberCount
                 << ",\"member_max\":2";
            if (res.controller) {
                resp << ",\"controller\":" << peerJson(*res.controller);
            }
            if (res.controlled) {
                resp << ",\"controlled\":" << peerJson(*res.controlled);
            }
            if (!res.notify.empty()) {
                resp << ",\"notify\":\"" << StringUtils::jsonEscape(res.notify) << "\"";
            }
            resp << "}";
            return resp.str();
        });

    // ── 获取对方地址 + 续期 ?room=XXX&uid=YYY ──
    server.route("GET", "/api/peer/peers",
        [&](const std::string &, const std::string &query, const std::string &) {
            std::string code = StringUtils::queryVal(query, "room");
            std::string uid = StringUtils::queryVal(query, "uid");
            if (code.empty())
                return AuthService::jsonErr("FIELDS_REQUIRED", "需要 room 参数");
            if (uid.empty())
                return AuthService::jsonErr("FIELDS_REQUIRED", "需要 uid 参数");

            auto res = store.getPeers(code, uid);
            if (!res.ok) return AuthService::jsonErr(res.errcode, res.msg);

            if (res.specialStatus == "peer_left")
                return std::string("{\"success\":true,\"status\":\"peer_left\",\"member_count\":1,\"member_max\":2}");
            if (res.specialStatus == "room_closed")
                return std::string("{\"success\":true,\"status\":\"room_closed\",\"member_count\":0,\"member_max\":2}");

            // 对方未加入 → 200 + peer:null (轮询静默等待)
            if (!res.peer)
                return std::string("{\"success\":true,\"peer\":null,\"member_count\":1,\"member_max\":2}");

            std::ostringstream resp;
            resp << "{\"success\":true"
                 << ",\"peer\":" << peerJson(*res.peer)
                 << ",\"member_count\":" << res.memberCount
                 << ",\"member_max\":2"
                 << "}";
            return resp.str();
        }, route::requireAuth(auth));

    // ── 推送数据进对端队列 {room_code, uid, op, x, y, split} ──
    server.route("POST", "/api/peer/push",
        [&](const std::string &body, const std::string &, const std::string &) {
            std::string code = StringUtils::jsonVal(body, "room_code");
            std::string uid = StringUtils::jsonVal(body, "uid");
            std::string op = StringUtils::jsonVal(body, "op");
            if (code.empty() || uid.empty() || op.empty())
                return AuthService::jsonErr("FIELDS_REQUIRED", "需要 room_code/uid/op");

            double x = 0, y = 0;
            try {
                x = std::stod(StringUtils::jsonVal(body, "x"));
                y = std::stod(StringUtils::jsonVal(body, "y"));
            } catch (...) { return AuthService::jsonErr("FIELDS_REQUIRED", "x/y 需为数字"); }
            bool split = parseBool(StringUtils::jsonVal(body, "split"));

            auto res = store.push(code, uid, op, x, y,
                ",\"split\":" + std::string(split ? "true" : "false"));
            if (!res.ok) return AuthService::jsonErr(res.errcode, res.msg);
            return AuthService::jsonOk("ok");
        }, route::requireAuth(auth));

    // ── 长轮询拉取 ?room=XXX&uid=YYY&seq=N&wait=100 ──
    server.route("GET", "/api/peer/pull",
        [&](const std::string &, const std::string &query, const std::string &) {
            std::string code = StringUtils::queryVal(query, "room");
            std::string uid = StringUtils::queryVal(query, "uid");
            long long lastSeq = 0;
            try { lastSeq = std::stoll(StringUtils::queryVal(query, "seq")); }
            catch (...) {}
            int waitMs = 100;
            try { waitMs = std::stoi(StringUtils::queryVal(query, "wait")); }
            catch (...) {}
            if (waitMs < 0) waitMs = 0;
            if (waitMs > 1000) waitMs = 1000;   // 上限 1s

            if (code.empty() || uid.empty())
                return AuthService::jsonErr("FIELDS_REQUIRED", "需要 room/uid 参数");

            auto res = store.pull(code, uid, lastSeq, waitMs);

            if (res.hasData) {
                std::ostringstream resp;
                resp << "{\"success\":true"
                     << ",\"has_data\":true"
                     << ",\"data\":" << res.data
                     << "}";
                return resp.str();
            }
            if (res.peerLeft)
                return std::string("{\"success\":true,\"peer_left\":true}");
            if (res.roomClosed)
                return std::string("{\"success\":true,\"room_closed\":true}");
            return std::string("{\"success\":true,\"has_data\":false}");
        }, route::requireAuth(auth));

    // ── 退出 {room_code, uid} ──
    server.route("POST", "/api/peer/unregister",
        [&](const std::string &body, const std::string &, const std::string &) {
            const auto &tp = route::currentUser();

            std::string code = StringUtils::jsonVal(body, "room_code");
            std::string uid = StringUtils::jsonVal(body, "uid");
            if (code.empty())
                return AuthService::jsonErr("FIELDS_REQUIRED", "需要 room_code");
            if (uid.empty())
                return AuthService::jsonErr("FIELDS_REQUIRED", "需要 uid");

            auto res = store.unregister(code, uid, tp.email);
            // 不在房间也幂等返回成功 (与旧实现一致)
            if (!res.wasInRoom) return AuthService::jsonOk("ok");
            return AuthService::jsonOk(res.closedRoom ? "房间已关闭" : "已退出房间");
        }, route::requireAuth(auth));

    // ── 续期 {room_code, uid} (兼容保留; 活跃续期已由 peers/push/pull 覆盖) ──
    server.route("POST", "/api/peer/heartbeat",
        [&](const std::string &body, const std::string &, const std::string &) {
            std::string code = StringUtils::jsonVal(body, "room_code");
            std::string uid = StringUtils::jsonVal(body, "uid");
            if (code.empty() || uid.empty())
                return AuthService::jsonErr("FIELDS_REQUIRED", "需要 room_code 和 uid");

            auto res = store.heartbeat(code, uid);
            if (!res.ok) return AuthService::jsonErr(res.errcode, res.msg);

            std::ostringstream resp;
            resp << "{\"success\":true,\"member_count\":" << res.memberCount << ",\"member_max\":2}";
            return resp.str();
        }, route::requireAuth(auth));
}
