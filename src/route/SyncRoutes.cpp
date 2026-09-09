#include "SyncRoutes.h"
#include "RouteGuard.h"
#include "../core/server/HttpServer.h"
#include "../auth/AuthService.h"
#include "../common/StringUtils.h"
#include "../peer/PeerStore.h"
#include <sstream>
#include <chrono>

// ═══════════════════════════════════════════════════════════════
//  /api/sync/* — 远程控制 A 套接口 (2026-09-10 合并: 存储统一到 PeerStore
//  Redis 键族, 原 SyncService(MySQL+内存条件变量) 已删除)。
//  本层职责: 参数解析 → PeerStore (uid=email 定位) → 组旧版响应格式
//  (sync 客户端零改动, 响应字段/错误码逐项映射, 差异见 docs/sync-peer.md)。
// ═══════════════════════════════════════════════════════════════

void registerSyncRoutes(HttpServer &server, AuthService &auth) {
    static PeerStore store;

    // ── 创建房间（创建者自动成为控制方）────────────
    server.route("POST", "/api/sync/create",
        [&](const std::string &body, const std::string &, const std::string &) {
            const auto &tp = route::currentUser();
            std::string roomName = StringUtils::jsonVal(body, "room_name");
            // addr 空串: sync 走服务器中转, 无 P2P 地址交换
            auto res = store.createRoom(tp.email, "", tp.email, roomName);
            if (!res.ok) return AuthService::jsonErr(res.errcode, res.msg);

            std::string label = roomName.empty() ? res.roomCode : roomName;
            std::ostringstream resp;
            resp << "{\"success\":true"
                 << ",\"room_code\":\"" << res.roomCode << "\""
                 << ",\"room_name\":\"" << AuthService::jsonEscape(label) << "\""
                 << ",\"your_role\":\"controller\""
                 << ",\"member_count\":1,\"member_max\":2"
                 << "}";
            return resp.str();
        }, route::requireAuth(auth));

    // ── 加入房间 ──────────────────────────────────
    server.route("POST", "/api/sync/join",
        [&](const std::string &body, const std::string &, const std::string &) {
            const auto &tp = route::currentUser();
            std::string code = StringUtils::jsonVal(body, "room_code");
            if (code.empty())
                return AuthService::jsonErr("FIELDS_REQUIRED", "房间码和邮箱不能为空");

            auto res = store.joinRoom(code, "", tp.email);
            if (!res.ok) {
                // errcode 翻译: peer 错误码 → sync 旧错误码 (客户端分支兼容)
                if (res.errcode == "SELF_JOIN")
                    return AuthService::jsonErr("SELF_JOIN", "不能加入自己创建的房间");
                if (res.errcode == "ROOM_FULL")
                    return AuthService::jsonErr("ROOM_FULL", "房间已满");
                return AuthService::jsonErr("ROOM_NOT_FOUND", "房间不存在");
            }

            // 控制方 email/房间名: 从键族读回 (sync 语义下 uid0 即邮箱)
            std::string controllerEmail, roomName;
            {
                auto st = store.roomStatus(code);
                roomName = st.roomName;
                controllerEmail = st.controllerUid;
            }

            std::ostringstream resp;
            resp << "{\"success\":true"
                 << ",\"room_code\":\"" << code << "\""
                 << ",\"room_name\":\"" << AuthService::jsonEscape(roomName) << "\""
                 << ",\"your_role\":\"controlled\""
                 << ",\"controller_email\":\"" << AuthService::jsonEscape(controllerEmail) << "\""
                 << ",\"member_count\":2,\"member_max\":2"
                 << "}";
            return resp.str();
        }, route::requireAuth(auth));

    // ── 退出房间 ──────────────────────────────────
    server.route("POST", "/api/sync/leave",
        [&](const std::string &body, const std::string &, const std::string &) {
            const auto &tp = route::currentUser();
            std::string code = StringUtils::jsonVal(body, "room_code");
            if (code.empty())
                return AuthService::jsonErr("FIELDS_REQUIRED", "参数不完整");

            auto res = store.unregister(code, tp.email, tp.email);
            // 旧语义: 控制方退出=关房; 受控方退出=已退出; 非成员=NOT_MEMBER
            // 新语义: 不在房间幂等返回 ok (差异见 docs/sync-peer.md)
            if (!res.wasInRoom) return AuthService::jsonOk("已退出房间");
            return AuthService::jsonOk(res.closedRoom ? "房间已关闭" : "已退出房间");
        }, route::requireAuth(auth));

    // ── 房间状态 (公开, 旧实现同样无鉴权) ──────────
    server.route("GET", "/api/sync/room/status",
        [&](const std::string &, const std::string &query, const std::string &) {
            std::string code = StringUtils::queryVal(query, "code");
            if (code.empty())
                return AuthService::jsonErr("FIELDS_REQUIRED", "缺少房间码");

            auto res = store.roomStatus(code);
            if (res.memberCount == 0)
                return AuthService::jsonErr("ROOM_NOT_FOUND", "房间不存在");

            // status 数字映射 (旧: 0=待加入 1=对局中 2=关闭):
            //   1人=待加入(0) 2人=对局中(1); 0人已在上方拦截
            int status = res.memberCount == 2 ? 1 : 0;

            std::ostringstream resp;
            resp << "{\"success\":true"
                 << ",\"room_code\":\"" << code << "\""
                 << ",\"room_name\":\"" << AuthService::jsonEscape(res.roomName) << "\""
                 << ",\"creator_email\":\"" << AuthService::jsonEscape(res.controllerUid) << "\""
                 << ",\"controller_email\":\"" << AuthService::jsonEscape(res.controllerUid) << "\""
                 << ",\"controlled_email\":\"" << AuthService::jsonEscape(res.controlledUid) << "\""
                 << ",\"member_count\":" << res.memberCount
                 << ",\"member_max\":2"
                 << ",\"status\":" << status
                 << "}";
            return resp.str();
        });

    // ── 控制方推送坐标（含授权：仅控制方能推送）────────
    server.route("POST", "/api/sync/push",
        [&](const std::string &body, const std::string &, const std::string &) {
            const auto &tp = route::currentUser();

            std::string roomCode = StringUtils::jsonVal(body, "room_code");
            std::string op       = StringUtils::jsonVal(body, "op");
            std::string xs       = StringUtils::jsonVal(body, "x");
            std::string ys       = StringUtils::jsonVal(body, "y");
            std::string delayMs  = StringUtils::jsonVal(body, "delay_ms");

            if (roomCode.empty())
                return AuthService::jsonErr("FIELDS_REQUIRED", "缺少房间码");

            double x = xs.empty() ? 0.0 : std::stod(xs);
            double y = ys.empty() ? 0.0 : std::stod(ys);
            // item 尾部字段: ts + extra (与旧 serializeCommand 字段一致)
            std::string extra = ",\"ts\":" + std::to_string(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            if (!delayMs.empty())
                extra += ",\"extra\":{\"delay_ms\":" + delayMs + "}";

            auto res = store.push(roomCode, tp.email, op, x, y, extra);
            if (!res.ok) {
                // 错误码翻译: 不在房间/未授权 → 旧 FORBIDDEN; 对端不在 → 旧实现
                // 也成功(瞬态数据), 丢弃即等价
                if (res.errcode == "PEER_LEFT")
                    return AuthService::jsonOk("坐标已同步");
                if (res.errcode == "NOT_FOUND" || res.errcode == "FORBIDDEN")
                    return AuthService::jsonErr("FORBIDDEN", "只有控制方能推送坐标");
                return AuthService::jsonErr(res.errcode, res.msg);
            }
            return AuthService::jsonOk("坐标已同步");
        }, route::requireAuth(auth));

    // ── 受控制方拉取坐标（长轮询，含授权：仅受控方能拉取）──
    server.route("GET", "/api/sync/pull",
        [&](const std::string &, const std::string &query, const std::string &) {
            const auto &tp = route::currentUser();

            std::string roomCode = StringUtils::queryVal(query, "code");
            std::string seqStr   = StringUtils::queryVal(query, "seq");
            std::string waitStr  = StringUtils::queryVal(query, "wait");

            if (roomCode.empty())
                return AuthService::jsonErr("FIELDS_REQUIRED", "缺少房间码");

            uint64_t lastSeq = seqStr.empty() ? 0 : std::stoull(seqStr);
            int      waitMs  = waitStr.empty() ? 5000 : std::stoi(waitStr);
            if (waitMs > 30000) waitMs = 30000;

            auto res = store.pull(roomCode, tp.email, (long long)lastSeq, waitMs);

            if (res.hasData)
                return "{\"success\":true,\"has_data\":true,\"data\":" + res.data + "}";
            if (res.peerLeft || res.roomClosed)
                // 房间关闭/对方离开: 旧实现条件变量超时也返回 has_data:false,
                // 客户端靠心跳/重连感知, 保持 has_data:false 语义
                return std::string("{\"success\":true,\"has_data\":false}");
            return std::string("{\"success\":true,\"has_data\":false}");
        }, route::requireAuth(auth));
}
