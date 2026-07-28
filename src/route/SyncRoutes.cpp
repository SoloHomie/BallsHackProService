#include "SyncRoutes.h"
#include "../core/server/HttpServer.h"
#include "../auth/AuthService.h"
#include "../sync/SyncService.h"
#include "../common/StringUtils.h"

void registerSyncRoutes(HttpServer &server, AuthService &auth, SyncService &sync) {

    // ── 创建房间 ──────────────────────────────────
    server.route("POST", "/api/sync/create",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
            return sync.createRoom(StringUtils::jsonVal(body, "room_name"), tp->email);
        });

    // ── 加入房间 ──────────────────────────────────
    server.route("POST", "/api/sync/join",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
            return sync.joinRoom(StringUtils::jsonVal(body, "room_code"), tp->email,
                                 StringUtils::jsonVal(body, "role"));
        });

    // ── 退出房间 ──────────────────────────────────
    server.route("POST", "/api/sync/leave",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
            return sync.leaveRoom(StringUtils::jsonVal(body, "room_code"), tp->email);
        });

    // ── 房间状态 ──────────────────────────────────
    server.route("GET", "/api/sync/room/status",
        [&](const std::string &, const std::string &query, const std::string &) {
            return sync.roomStatus(StringUtils::queryVal(query, "code"));
        });

    // ── 控制方推送坐标（写入内存并唤醒等待中的受控制方）──
    server.route("POST", "/api/sync/push",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");

            std::string roomCode = StringUtils::jsonVal(body, "room_code");
            std::string op       = StringUtils::jsonVal(body, "op");
            std::string xs       = StringUtils::jsonVal(body, "x");
            std::string ys       = StringUtils::jsonVal(body, "y");
            std::string delayMs  = StringUtils::jsonVal(body, "delay_ms");

            if (roomCode.empty())
                return AuthService::jsonErr("FIELDS_REQUIRED", "缺少房间码");

            double x = xs.empty() ? 0.0 : std::stod(xs);
            double y = ys.empty() ? 0.0 : std::stod(ys);
            std::string extra;
            if (!delayMs.empty())
                extra = "{\"delay_ms\":" + delayMs + "}";

            sync.pushCommand(roomCode, op, x, y, extra);
            return AuthService::jsonOk("坐标已同步");
        });

    // ── 受控制方拉取坐标（长轮询：阻塞等待直到有数据或超时）──
    server.route("GET", "/api/sync/pull",
        [&](const std::string &, const std::string &query, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");

            std::string roomCode = StringUtils::queryVal(query, "code");
            std::string seqStr   = StringUtils::queryVal(query, "seq");
            std::string waitStr  = StringUtils::queryVal(query, "wait");

            if (roomCode.empty())
                return AuthService::jsonErr("FIELDS_REQUIRED", "缺少房间码");

            uint64_t lastSeq = seqStr.empty() ? 0 : std::stoull(seqStr);
            int      waitMs  = waitStr.empty() ? 5000 : std::stoi(waitStr);
            if (waitMs > 30000) waitMs = 30000;  // 最长 30s

            std::string cmd = sync.pullCommand(roomCode, lastSeq, waitMs);
            if (cmd.empty())
                return std::string("{\"success\":true,\"has_data\":false}");

            return "{\"success\":true,\"has_data\":true,\"data\":" + cmd + "}";
        });
}
