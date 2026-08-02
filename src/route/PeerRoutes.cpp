#include "PeerRoutes.h"
#include "../core/server/HttpServer.h"
#include "../auth/AuthService.h"
#include "../core/cache/RedisClient.h"
#include "../common/StringUtils.h"
#include <string>
#include <sstream>
#include <cstdlib>
#include <ctime>

// ═══════════════════════════════════════════════════════════════
//  P2P 信令服务器 — Redis 房间管理
// ═══════════════════════════════════════════════════════════════
//
// Key:  peer:<code>:0     → 控制方 ip:port
//       peer:<code>:1     → 被控制方 ip:port
//       peer:<code>:email → 控制方 email（防自加入）
//
// TTL: 120s，通过 /api/peer/peers 轮询续期
//
// 流程:
//   控制方 POST /api/peer/create  {port}               → 生成房码 + slot0
//   被方   POST /api/peer/join    {room_code, port}    → slot1 + 返回对方地址
//   双方   GET  /api/peer/status  ?room=XXX             → 查看人数/状态
//   双方   GET  /api/peer/peers   ?room=XXX&self=ADDR   → 获取对方地址（同时续期）
//   任一方 POST /api/peer/unregister {room_code, addr}  → 退出
// ═══════════════════════════════════════════════════════════════

static const int  ROOM_TTL  = 120;  // 房间槽位 TTL（秒）
static const int  NOTIFY_TTL = 30;  // 离开通知 TTL
static const char RC_CHARS[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
static const int  RC_LEN = sizeof(RC_CHARS) - 1;

// ── 6位房间码生成 ──
static std::string generateRoomCode() {
    std::string code(6, '0');
    for (int i = 0; i < 6; ++i)
        code[i] = RC_CHARS[rand() % RC_LEN];
    return code;
}

// ── 提取客户端真实 IP ──
// Cloudflare → Caddy → App
static std::string extractClientIp(const std::string &headers) {
    auto getHdr = [&](const std::string &name) -> std::string {
        // 精确匹配 "Name: value\r\n"
        std::string key = name + ": ";
        auto pos = headers.find(key);
        if (pos == std::string::npos) {
            std::string lower = name;
            for (auto &c : lower) c = (c >= 'A' && c <= 'Z') ? c + 32 : c;
            key = lower + ": ";
            pos = headers.find(key);
            if (pos == std::string::npos) return {};
        }
        auto start = pos + key.size();
        auto end = headers.find("\r\n", start);
        if (end == std::string::npos) end = headers.size();
        return headers.substr(start, end - start);
    };

    std::string ip;
    // 优先级: CF-Connecting-IP > X-Forwarded-For (第一个是真实客户端) > X-Real-IP
    ip = getHdr("CF-Connecting-IP");
    if (ip.empty()) {
        std::string xff = getHdr("X-Forwarded-For");
        if (!xff.empty()) {
            auto comma = xff.find(',');
            ip = (comma != std::string::npos) ? xff.substr(0, comma) : xff;
        }
    }
    if (ip.empty()) ip = getHdr("X-Real-IP");
    // 去空格
    while (!ip.empty() && ip.front() == ' ') ip.erase(0, 1);
    while (!ip.empty() && ip.back() == ' ') ip.pop_back();
    if (ip.empty()) ip = "127.0.0.1";
    return ip;
}

// ── 从 "ip:port" 字符串提取 IP 和 port ──
static bool parseAddr(const std::string &val, std::string &ip, int &port) {
    auto colon = val.rfind(':');  // 最后一个 : 才是端口分隔符（IPv6 兼容）
    if (colon == std::string::npos) return false;
    ip = val.substr(0, colon);
    if (!ip.empty() && ip.front() == '[') ip = ip.substr(1, ip.size() - 2);  // [::1]:port 格式
    try { port = std::stoi(val.substr(colon + 1)); }
    catch (...) { return false; }
    return port > 0;
}

// ── 构建 peer JSON ──
static std::string peerJson(const std::string &ip, int port) {
    std::ostringstream s;
    s << "{\"ip\":\"" << ip << "\",\"port\":" << port << "}";
    return s.str();
}

// ── 构建 peer JSON 从 "ip:port" ──
static std::string peerJsonFromVal(const std::string &val) {
    std::string ip; int port;
    if (parseAddr(val, ip, port))
        return peerJson(ip, port);
    return "null";
}

// 端口匹配：客户端用 127.0.0.1，服务器存的是公网 IP
static std::string _portOf(const std::string &addr) {
    if (addr.empty()) return "";
    auto pos = addr.rfind(':');
    return pos != std::string::npos ? addr.substr(pos) : "";
}

void registerPeerRoutes(HttpServer &server, AuthService &auth, DbPool &) {

    // ═══════════════════════════════════════════════════════
    //  POST /api/peer/create
    //  控制方创建房间
    //  Body: {port: 12345}
    //  ═══════════════════════════════════════════════════════
    server.route("POST", "/api/peer/create",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");

            std::string portStr = StringUtils::jsonVal(body, "port");
            int port = 0;
            try { port = std::stoi(portStr); }
            catch (...) { return AuthService::jsonErr("FIELDS_REQUIRED", "需要有效 port"); }
            if (port <= 0 || port > 65535)
                return AuthService::jsonErr("FIELDS_REQUIRED", "port 范围 1-65535");

            std::string clientIp = extractClientIp(headers);
            std::string val = clientIp + ":" + std::to_string(port);
            std::string email = tp->email;

            auto &r = RedisClient::instance();

            // 如果用户已有活跃房间，先清理旧房间
            {
                auto existing = r.get("peer:user:" + email);
                if (existing) {
                    std::string oldCode = *existing;
                    r.del("peer:" + oldCode + ":0");
                    r.del("peer:" + oldCode + ":1");
                    r.del("peer:" + oldCode + ":email");
                    r.setex("peer:" + oldCode + ":notify", NOTIFY_TTL, "room_closed");
                    r.del("peer:user:" + email);
                }
            }

            // 生成唯一房间码
            std::string code;
            for (int retry = 0; retry < 20; ++retry) {
                code = generateRoomCode();
                // SETNX 防止房间码冲突
                if (r.setnx("peer:" + code + ":lock", "1")) {
                    r.expire("peer:" + code + ":lock", 5);
                    break;
                }
                code.clear();
            }
            if (code.empty())
                return AuthService::jsonErr("ROOM_CREATE_FAILED", "生成房间码失败，请重试");

            // 写 slot 0（控制方）
            r.setex("peer:" + code + ":0", ROOM_TTL, val);
            r.setex("peer:" + code + ":email", ROOM_TTL, email);
            r.setex("peer:user:" + email, ROOM_TTL, code);

            std::ostringstream resp;
            resp << "{\"success\":true"
                 << ",\"room_code\":\"" << code << "\""
                 << ",\"member_count\":1"
                 << ",\"member_max\":2"
                 << ",\"role\":\"controller\""
                 << "}";
            return resp.str();
        });

    // ═══════════════════════════════════════════════════════
    //  POST /api/peer/join
    //  被控制方加入房间
    //  Body: {room_code: "ABC123", port: 12346}
    //  ═══════════════════════════════════════════════════════
    server.route("POST", "/api/peer/join",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");

            std::string code = StringUtils::jsonVal(body, "room_code");
            std::string portStr = StringUtils::jsonVal(body, "port");
            int port = 0;
            try { port = std::stoi(portStr); }
            catch (...) { return AuthService::jsonErr("FIELDS_REQUIRED", "需要有效 port"); }
            if (code.empty() || port <= 0 || port > 65535)
                return AuthService::jsonErr("FIELDS_REQUIRED", "需要 room_code 和有效 port");

            std::string clientIp = extractClientIp(headers);
            std::string val = clientIp + ":" + std::to_string(port);
            std::string email = tp->email;

            auto &r = RedisClient::instance();

            // 检查房间是否存在
            auto v0 = r.get("peer:" + code + ":0");
            if (!v0)
                return AuthService::jsonErr("ROOM_NOT_FOUND", "房间不存在或已过期");

            // 检查是否加入自己的房间（通过 email 判断，而非 IP）
            auto ownerEmail = r.get("peer:" + code + ":email");
            if (ownerEmail && *ownerEmail == email)
                return AuthService::jsonErr("SELF_JOIN", "不能加入自己创建的房间");

            // 检查是否已有其他用户加入了被控制方槽位
            auto v1 = r.get("peer:" + code + ":1");
            if (v1) {
                // 检查是不是自己（重连）
                if (*v1 == val) {
                    // 重连：更新 TTL
                    r.setex("peer:" + code + ":1", ROOM_TTL, val);
                    r.setex("peer:" + code + ":0", ROOM_TTL, *v0);
                    std::string peerIp; int peerPort;
                    if (parseAddr(*v0, peerIp, peerPort)) {
                        std::ostringstream resp;
                        resp << "{\"success\":true"
                             << ",\"reconnect\":true"
                             << ",\"peer\":" << peerJson(peerIp, peerPort)
                             << ",\"member_count\":2"
                             << ",\"member_max\":2"
                             << "}";
                        return resp.str();
                    }
                }
                return AuthService::jsonErr("ROOM_FULL", "房间已满");
            }

            // 写入 slot 1（被控制方）
            r.setex("peer:" + code + ":1", ROOM_TTL, val);
            // 续期 slot 0
            r.expire("peer:" + code + ":0", ROOM_TTL);
            r.expire("peer:" + code + ":email", ROOM_TTL);

            // 返回对方（控制方）地址
            std::string peerIp; int peerPort;
            if (!parseAddr(*v0, peerIp, peerPort))
                return AuthService::jsonErr("INVALID", "控制方数据损坏");

            std::ostringstream resp;
            resp << "{\"success\":true"
                 << ",\"peer\":" << peerJson(peerIp, peerPort)
                 << ",\"member_count\":2"
                 << ",\"member_max\":2"
                 << ",\"role\":\"controlled\""
                 << "}";
            return resp.str();
        });

    // ═══════════════════════════════════════════════════════
    //  GET /api/peer/status
    //  查看房间连接状态
    //  Query: ?room=ABC123
    //  ═══════════════════════════════════════════════════════
    server.route("GET", "/api/peer/status",
        [&](const std::string &, const std::string &query, const std::string &) {
            std::string code = StringUtils::queryVal(query, "room");
            if (code.empty())
                return AuthService::jsonErr("FIELDS_REQUIRED", "缺少 room 参数");

            auto &r = RedisClient::instance();
            auto v0 = r.get("peer:" + code + ":0");
            auto v1 = r.get("peer:" + code + ":1");

            int count = 0;
            if (v0) count++;
            if (v1) count++;

            // 检查离开通知
            auto notify = r.get("peer:" + code + ":notify");
            std::string notifyMsg;
            if (notify) {
                notifyMsg = *notify;
                r.del("peer:" + code + ":notify");
            }

            std::ostringstream resp;
            resp << "{\"success\":true"
                 << ",\"room_code\":\"" << code << "\""
                 << ",\"member_count\":" << count
                 << ",\"member_max\":2";
            if (v0) {
                resp << ",\"controller\":" << peerJsonFromVal(*v0);
            }
            if (v1) {
                resp << ",\"controlled\":" << peerJsonFromVal(*v1);
            }
            if (!notifyMsg.empty()) {
                resp << ",\"notify\":\"" << notifyMsg << "\"";
            }
            resp << "}";
            return resp.str();
        });

    // ═══════════════════════════════════════════════════════
    //  GET /api/peer/peers
    //  获取对方地址 + 续期自身槽位
    //  Query: ?room=ABC123&self=ip:port
    //  ═══════════════════════════════════════════════════════
    server.route("GET", "/api/peer/peers",
        [&](const std::string &, const std::string &query, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");

            std::string code = StringUtils::queryVal(query, "room");
            std::string myAddr = StringUtils::queryVal(query, "self");
            if (code.empty())
                return AuthService::jsonErr("FIELDS_REQUIRED", "需要 room 参数");

            auto &r = RedisClient::instance();

            // 检查对方是否离开了
            auto notify = r.get("peer:" + code + ":notify");
            if (notify) {
                std::string msg = *notify;
                r.del("peer:" + code + ":notify");
                if (msg == "peer_left")
                    return std::string("{\"success\":true,\"status\":\"peer_left\",\"message\":\"对方已退出房间\"}");
                if (msg == "room_closed")
                    return std::string("{\"success\":true,\"status\":\"room_closed\",\"message\":\"房间已关闭\"}");
            }

            auto v0 = r.get("peer:" + code + ":0");
            auto v1 = r.get("peer:" + code + ":1");

            if (!v0 && !v1)
                return AuthService::jsonErr("ROOM_NOT_FOUND", "房间不存在或已过期");

            // 续期自己所在的槽位
            if (!myAddr.empty()) {
                if (v0 && _portOf(*v0) == _portOf(myAddr)) {
                    r.expire("peer:" + code + ":0", ROOM_TTL);
                    r.expire("peer:" + code + ":email", ROOM_TTL);
                } else if (v1 && _portOf(*v1) == _portOf(myAddr)) {
                    r.expire("peer:" + code + ":1", ROOM_TTL);
                }
            }

            // 提取端口匹配（客户端用127.0.0.1，服务器存的是公网IP）
            auto portOf = [](const std::string &addr) -> std::string {
                auto pos = addr.rfind(':');
                return pos != std::string::npos ? addr.substr(pos) : "";
            };
            std::string myPort = portOf(myAddr);

            // 返回不是自己的那个
            std::string result;
            if (v0 && portOf(*v0) != myPort)
                result = *v0;
            else if (v1 && portOf(*v1) != myPort)
                result = *v1;
            else if (v0 && v1) {
                result = *v1;
            }

            if (result.empty())
                return std::string("{\"success\":true,\"status\":\"waiting\",\"member_count\":" + std::to_string(v1 ? 2 : 1) + "}");

            std::string peerIp; int peerPort;
            if (!parseAddr(result, peerIp, peerPort))
                return AuthService::jsonErr("INVALID", "数据损坏");

            int count = 0;
            if (v0) count++;
            if (v1) count++;

            std::ostringstream resp;
            resp << "{\"success\":true"
                 << ",\"peer\":" << peerJson(peerIp, peerPort)
                 << ",\"member_count\":" << count
                 << ",\"member_max\":2"
                 << "}";
            return resp.str();
        });

    // ═══════════════════════════════════════════════════════
    //  POST /api/peer/unregister
    //  退出房间 / 取消注册
    //  Body: {room_code: "ABC123", addr: "ip:port"}
    //  ═══════════════════════════════════════════════════════
    server.route("POST", "/api/peer/unregister",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");

            std::string code = StringUtils::jsonVal(body, "room_code");
            std::string myAddr = StringUtils::jsonVal(body, "addr");
            if (code.empty())
                return AuthService::jsonErr("FIELDS_REQUIRED", "需要 room_code");

            auto &r = RedisClient::instance();
            std::string k0 = "peer:" + code + ":0";
            std::string k1 = "peer:" + code + ":1";

            auto v0 = r.get(k0);
            auto v1 = r.get(k1);

            bool isController = false;
            if (v0 && _portOf(*v0) == _portOf(myAddr)) {
                isController = true;
                r.del(k0);
                r.del("peer:" + code + ":email");
                // 控制器退出 → 关闭房间，通知被控制方
                r.setex("peer:" + code + ":notify", NOTIFY_TTL, "room_closed");
                if (v1) r.del(k1);
            } else if (v1 && _portOf(*v1) == _portOf(myAddr)) {
                r.del(k1);
                // 被控制方退出 → 通知控制方
                r.setex("peer:" + code + ":notify", NOTIFY_TTL, "peer_left");
            }

            // 清理用户→房间映射
            r.del("peer:user:" + tp->email);

            return AuthService::jsonOk(isController ? "房间已关闭" : "已退出房间");
        });

    // ═══════════════════════════════════════════════════════
    //  POST /api/peer/heartbeat
    //  续期自身槽位（控制方定期调用保持房间存活）
    //  Body: {room_code: "ABC123", addr: "ip:port"}
    //  ═══════════════════════════════════════════════════════
    server.route("POST", "/api/peer/heartbeat",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");

            std::string code = StringUtils::jsonVal(body, "room_code");
            std::string myAddr = StringUtils::jsonVal(body, "addr");
            if (code.empty() || myAddr.empty())
                return AuthService::jsonErr("FIELDS_REQUIRED", "需要 room_code 和 addr");

            auto &r = RedisClient::instance();

            // 续期自己所在的槽位
            auto v0 = r.get("peer:" + code + ":0");
            auto v1 = r.get("peer:" + code + ":1");

            if (v0 && _portOf(*v0) == _portOf(myAddr)) {
                r.expire("peer:" + code + ":0", ROOM_TTL);
                r.expire("peer:" + code + ":email", ROOM_TTL);
            } else if (v1 && _portOf(*v1) == _portOf(myAddr)) {
                r.expire("peer:" + code + ":1", ROOM_TTL);
            } else {
                return std::string("{\"success\":true,\"status\":\"not_found\"}");
            }

            int count = 0;
            if (v0) count++;
            if (v1) count++;

            std::ostringstream resp;
            resp << "{\"success\":true,\"member_count\":" << count << ",\"member_max\":2}";
            return resp.str();
        });
}
