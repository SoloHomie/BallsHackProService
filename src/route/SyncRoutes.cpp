#include "SyncRoutes.h"
#include "../core/server/HttpServer.h"
#include "../auth/AuthService.h"
#include "../sync/SyncService.h"
#include "../common/StringUtils.h"

void registerSyncRoutes(HttpServer &server, AuthService &auth, SyncService &sync) {

    // ── 创建房间（创建者自动成为控制方）────────────
    server.route("POST", "/api/sync/create",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
            std::string roomName = StringUtils::jsonVal(body, "room_name");
            return sync.createRoom(roomName, tp->email);
        });

    // ── 加入房间 ──────────────────────────────────
    server.route("POST", "/api/sync/join",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
            return sync.joinRoom(
                StringUtils::jsonVal(body, "room_code"), tp->email);
        });

    // ── 退出房间 ──────────────────────────────────
    server.route("POST", "/api/sync/leave",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");
            return sync.leaveRoom(
                StringUtils::jsonVal(body, "room_code"), tp->email);
        });

    // ── 房间状态 ──────────────────────────────────
    server.route("GET", "/api/sync/room/status",
        [&](const std::string &, const std::string &query, const std::string &) {
            return sync.roomStatus(StringUtils::queryVal(query, "code"));
        });

    // ── 控制方推送坐标（含授权：仅控制方能推送）────────
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

            return sync.pushCommand(roomCode, tp->email, op, x, y, extra);
        });

    // ── 受控制方拉取坐标（长轮询，含授权：仅受控方能拉取）──
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
            if (waitMs > 30000) waitMs = 30000;

            std::string cmd = sync.pullCommand(roomCode, tp->email, lastSeq, waitMs);
            if (cmd.empty())
                return std::string("{\"success\":true,\"has_data\":false}");
            if (cmd[0] == '{' && cmd.find("\"errcode\"") != std::string::npos)
                return cmd;  // 错误响应直接返回

            return "{\"success\":true,\"has_data\":true,\"data\":" + cmd + "}";
        });
}
