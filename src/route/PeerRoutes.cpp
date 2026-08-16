#include "PeerRoutes.h"
#include "../core/server/HttpServer.h"
#include "../auth/AuthService.h"
#include "../core/cache/RedisClient.h"
#include "../common/StringUtils.h"
#include <string>
#include <vector>
#include <iterator>
#include <sstream>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <thread>

// ═══════════════════════════════════════════════════════════════
//  P2P 信令 + Relay 中转服务器 — Redis 房间管理 (v2, 2026-08-14)
// ═══════════════════════════════════════════════════════════════
//
// 槽位按"设备 uid"定位 (每台机器持久化一个随机 uid):
//   peer:<code>:0     → 控制方 public_ip:port
//   peer:<code>:uid0  → 控制方 uid
//   peer:<code>:1     → 被控制方 public_ip:port
//   peer:<code>:uid1  → 被控制方 uid
//   peer:<code>:email → 控制方 email (旧房间清理用)
//   relay:<code>:0/1  → 中转数据队列 (按目标槽位分列)
//   relay:<code>:seq  → 全局数据序号
//
// 不再比对 "ip:port" 字符串 (客户端无法自know公网 IP, 曾致匹配全错位)。
// 同账号两台机器可互连 (uid 不同即可); 同一设备 (uid 相同) 禁止自加入。
//
// TTL: 120s，peers/push/pull 活跃即续期。
//
// 流程:
//   控制方 POST /api/peer/create  {port, uid}            → 房码 + slot0
//   被方   POST /api/peer/join    {room_code, port, uid} → slot1 + 返回对方地址
//   双方   GET  /api/peer/status  ?room=XXX               → 查看人数/状态
//   双方   GET  /api/peer/peers   ?room=XXX&uid=YYY       → 获取对方地址 + 续期
//   控制方 POST /api/peer/push    {room_code, uid, op, x, y, split} → 写入对端队列
//   双方   GET  /api/peer/pull    ?room=XXX&uid=YYY&seq=N&wait=100 → 长轮询取数据
//   任一方 POST /api/peer/unregister {room_code, uid}     → 退出
//   POST /api/peer/heartbeat {room_code, uid}             → 续期 (兼容保留)
// ═══════════════════════════════════════════════════════════════

static const int  ROOM_TTL    = 120;  // 房间槽位 TTL（秒）
static const int  RELAY_TTL   = 300;  // 中转队列/序号 TTL（秒）
static const int  NOTIFY_TTL  = 30;   // 离开通知 TTL
static const long RELAY_MAX   = 100;  // 每队列最多保留条数
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

// ── 从 "ip:port" 字符串提取 IP 和 port ──
static bool parseAddr(const std::string &val, std::string &ip, int &port) {
    auto colon = val.find(':');
    if (colon == std::string::npos) return false;
    ip = val.substr(0, colon);
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

// ── 取队列里 seq > lastSeq 的第一条 (顺序扫描, 队列上限 100) ──
static bool relayPopNext(sw::redis::Redis &r, const std::string &listKey,
                         long long lastSeq, std::string &out) {
    std::vector<std::string> items;
    r.lrange(listKey, 0, -1, std::back_inserter(items));
    for (const auto &it : items) {
        std::string s = StringUtils::jsonVal(it, "seq");
        try {
            if (std::stoll(s) > lastSeq) { out = it; return true; }
        } catch (...) {}
    }
    return false;
}

// ── 解析 bool (兼容 "true"/"1"/true) ──
static bool parseBool(const std::string &s) {
    return s == "true" || s == "1" || s == "TRUE" || s == "True";
}

void registerPeerRoutes(HttpServer &server, AuthService &auth, DbPool &) {

    // ═══════════════════════════════════════════════════════
    //  POST /api/peer/create
    //  控制方创建房间
    //  Body: {port: 12345, uid: "设备唯一标识"}
    //  ═══════════════════════════════════════════════════════
    server.route("POST", "/api/peer/create",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");

            std::string portStr = StringUtils::jsonVal(body, "port");
            std::string uid = StringUtils::jsonVal(body, "uid");
            int port = 0;
            try { port = std::stoi(portStr); }
            catch (...) { return AuthService::jsonErr("FIELDS_REQUIRED", "需要有效 port"); }
            if (port <= 0 || port > 65535)
                return AuthService::jsonErr("FIELDS_REQUIRED", "port 范围 1-65535");
            if (uid.empty() || uid.size() > 64)
                return AuthService::jsonErr("FIELDS_REQUIRED", "需要有效 uid");

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
                    r.del("peer:" + oldCode + ":uid0");
                    r.del("peer:" + oldCode + ":uid1");
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
            r.setex("peer:" + code + ":uid0", ROOM_TTL, uid);
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
    //  Body: {room_code: "ABC123", port: 12346, uid: "设备唯一标识"}
    //  ═══════════════════════════════════════════════════════
    server.route("POST", "/api/peer/join",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");

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

            std::string clientIp = extractClientIp(headers);
            std::string val = clientIp + ":" + std::to_string(port);

            auto &r = RedisClient::instance();

            // 检查房间是否存在
            auto v0 = r.get("peer:" + code + ":0");
            if (!v0)
                return AuthService::jsonErr("ROOM_NOT_FOUND", "房间不存在或已过期");

            // 同一设备不能加入自己创建的房间 (uid 相同; 同账号不同设备允许)
            auto uid0 = r.get("peer:" + code + ":uid0");
            if (uid0 && *uid0 == uid)
                return AuthService::jsonErr("SELF_JOIN", "同一设备不能加入自己创建的房间");

            // 检查是否已有其他设备占用了被控制方槽位
            auto v1 = r.get("peer:" + code + ":1");
            if (v1) {
                auto uid1 = r.get("peer:" + code + ":uid1");
                // 自己重连 (同 uid) → 更新 TTL
                if (uid1 && *uid1 == uid) {
                    r.setex("peer:" + code + ":1", ROOM_TTL, val);
                    r.setex("peer:" + code + ":uid1", ROOM_TTL, uid);
                    r.expire("peer:" + code + ":0", ROOM_TTL);
                    r.expire("peer:" + code + ":uid0", ROOM_TTL);
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
            r.setex("peer:" + code + ":uid1", ROOM_TTL, uid);
            // 续期 slot 0
            r.expire("peer:" + code + ":0", ROOM_TTL);
            r.expire("peer:" + code + ":uid0", ROOM_TTL);
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
    //  Query: ?room=ABC123&uid=YYY
    //  ═══════════════════════════════════════════════════════
    server.route("GET", "/api/peer/peers",
        [&](const std::string &, const std::string &query, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");

            std::string code = StringUtils::queryVal(query, "room");
            std::string uid = StringUtils::queryVal(query, "uid");
            if (code.empty())
                return AuthService::jsonErr("FIELDS_REQUIRED", "需要 room 参数");
            if (uid.empty())
                return AuthService::jsonErr("FIELDS_REQUIRED", "需要 uid 参数");

            auto &r = RedisClient::instance();

            // 检查对方是否离开了 (200+status, 客户端按 status 字段处理)
            auto notify = r.get("peer:" + code + ":notify");
            if (notify) {
                std::string msg = *notify;
                r.del("peer:" + code + ":notify");
                if (msg == "peer_left")
                    return std::string("{\"success\":true,\"status\":\"peer_left\",\"member_count\":1,\"member_max\":2}");
                if (msg == "room_closed")
                    return std::string("{\"success\":true,\"status\":\"room_closed\",\"member_count\":0,\"member_max\":2}");
            }

            auto v0 = r.get("peer:" + code + ":0");
            auto v1 = r.get("peer:" + code + ":1");

            if (!v0 && !v1)
                return AuthService::jsonErr("ROOM_NOT_FOUND", "房间不存在或已过期");

            // 按 uid 定位自己所在的槽位 (0=控制方, 1=被控制方)
            int mySlot = -1;
            {
                auto uid0 = r.get("peer:" + code + ":uid0");
                auto uid1 = r.get("peer:" + code + ":uid1");
                if (uid0 && *uid0 == uid && v0) mySlot = 0;
                else if (uid1 && *uid1 == uid && v1) mySlot = 1;
            }
            if (mySlot < 0)
                return AuthService::jsonErr("NOT_FOUND", "你的设备不在房间中，请重新加入");

            // 续期自己所在的槽位
            if (mySlot == 0) {
                r.expire("peer:" + code + ":0", ROOM_TTL);
                r.expire("peer:" + code + ":uid0", ROOM_TTL);
                r.expire("peer:" + code + ":email", ROOM_TTL);
            } else {
                r.expire("peer:" + code + ":1", ROOM_TTL);
                r.expire("peer:" + code + ":uid1", ROOM_TTL);
            }

            // 返回另一个槽位; 对方未加入 → 200 + peer:null (轮询静默等待)
            auto other = (mySlot == 0) ? v1 : v0;
            if (!other)
                return std::string("{\"success\":true,\"peer\":null,\"member_count\":1,\"member_max\":2}");

            std::string peerIp; int peerPort;
            if (!parseAddr(*other, peerIp, peerPort))
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
    //  POST /api/peer/push  (Relay 中转)
    //  控制方推送操作数据 → 写入对端队列
    //  Body: {room_code, uid, op, x, y, split}
    //  ═══════════════════════════════════════════════════════
    server.route("POST", "/api/peer/push",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");

            std::string code = StringUtils::jsonVal(body, "room_code");
            std::string uid = StringUtils::jsonVal(body, "uid");
            std::string op = StringUtils::jsonVal(body, "op");
            if (code.empty() || uid.empty() || op.empty())
                return AuthService::jsonErr("FIELDS_REQUIRED", "需要 room_code/uid/op");

            auto &r = RedisClient::instance();

            auto v0 = r.get("peer:" + code + ":0");
            auto v1 = r.get("peer:" + code + ":1");
            if (!v0 && !v1)
                return AuthService::jsonErr("ROOM_NOT_FOUND", "房间不存在或已过期");

            // 定位发送方槽位 → 数据进对端队列
            int mySlot = -1;
            {
                auto uid0 = r.get("peer:" + code + ":uid0");
                auto uid1 = r.get("peer:" + code + ":uid1");
                if (uid0 && *uid0 == uid && v0) mySlot = 0;
                else if (uid1 && *uid1 == uid && v1) mySlot = 1;
            }
            if (mySlot < 0)
                return AuthService::jsonErr("NOT_FOUND", "你的设备不在房间中，请重新加入");

            // 续期自身槽位 (活跃即保活)
            r.expire("peer:" + code + ":" + std::to_string(mySlot), ROOM_TTL);
            r.expire("peer:" + code + ":uid" + std::to_string(mySlot), ROOM_TTL);
            if (mySlot == 0) r.expire("peer:" + code + ":email", ROOM_TTL);

            // 对端不在线 → 数据直接丢弃 (操作坐标是瞬态数据)
            int dstSlot = (mySlot == 0) ? 1 : 0;
            auto dst = (dstSlot == 0) ? v0 : v1;
            if (!dst)
                return AuthService::jsonErr("PEER_LEFT", "对方已退出房间");

            // 数值解析
            double x = 0, y = 0;
            try {
                x = std::stod(StringUtils::jsonVal(body, "x"));
                y = std::stod(StringUtils::jsonVal(body, "y"));
            } catch (...) { return AuthService::jsonErr("FIELDS_REQUIRED", "x/y 需为数字"); }
            bool split = parseBool(StringUtils::jsonVal(body, "split"));

            // 全局序号
            long long seq = r.incr("relay:" + code + ":seq");
            r.expire("relay:" + code + ":seq", RELAY_TTL);

            std::ostringstream item;
            item << "{\"seq\":" << seq
                 << ",\"op\":\"" << AuthService::jsonEscape(op) << "\""
                 << ",\"x\":" << x
                 << ",\"y\":" << y
                 << ",\"split\":" << (split ? "true" : "false")
                 << "}";

            std::string listKey = "relay:" + code + ":" + std::to_string(dstSlot);
            r.rpush(listKey, item.str());
            r.ltrim(listKey, -RELAY_MAX, -1);   // 只留最近 100 条
            r.expire(listKey, RELAY_TTL);

            return AuthService::jsonOk("ok");
        });

    // ═══════════════════════════════════════════════════════
    //  GET /api/peer/pull  (Relay 中转, 长轮询)
    //  拉取本设备队列中 seq > N 的第一条数据
    //  Query: ?room=ABC123&uid=YYY&seq=N&wait=100
    //  ═══════════════════════════════════════════════════════
    server.route("GET", "/api/peer/pull",
        [&](const std::string &, const std::string &query, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");

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

            auto &r = RedisClient::instance();

            // 定位自身槽位
            auto v0 = r.get("peer:" + code + ":0");
            auto v1 = r.get("peer:" + code + ":1");
            int mySlot = -1;
            {
                auto uid0 = r.get("peer:" + code + ":uid0");
                auto uid1 = r.get("peer:" + code + ":uid1");
                if (uid0 && *uid0 == uid && v0) mySlot = 0;
                else if (uid1 && *uid1 == uid && v1) mySlot = 1;
            }
            if (mySlot < 0)
                return std::string("{\"success\":true,\"room_closed\":true}");

            std::string listKey = "relay:" + code + ":" + std::to_string(mySlot);

            // 长轮询: 最多等 waitMs
            auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(waitMs);
            while (true) {
                std::string item;
                if (relayPopNext(r, listKey, lastSeq, item)) {
                    std::ostringstream resp;
                    resp << "{\"success\":true"
                         << ",\"has_data\":true"
                         << ",\"data\":" << item
                         << "}";
                    return resp.str();
                }
                // 对端槽位消失 → 对方已退出 (我还在房间)
                if (!r.get("peer:" + code + ":" + std::to_string(1 - mySlot)))
                    return std::string("{\"success\":true,\"peer_left\":true}");
                // 我方槽位消失 → 房间整体关闭
                if (!r.get("peer:" + code + ":" + std::to_string(mySlot)))
                    return std::string("{\"success\":true,\"room_closed\":true}");
                if (std::chrono::steady_clock::now() >= deadline) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            // 无数据也要续期 (轮询即活跃)
            r.expire("peer:" + code + ":" + std::to_string(mySlot), ROOM_TTL);
            r.expire("peer:" + code + ":uid" + std::to_string(mySlot), ROOM_TTL);
            if (mySlot == 0) r.expire("peer:" + code + ":email", ROOM_TTL);

            return std::string("{\"success\":true,\"has_data\":false}");
        });

    // ═══════════════════════════════════════════════════════
    //  POST /api/peer/unregister
    //  退出房间 / 取消注册
    //  Body: {room_code: "ABC123", uid: "设备唯一标识"}
    //  ═══════════════════════════════════════════════════════
    server.route("POST", "/api/peer/unregister",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");

            std::string code = StringUtils::jsonVal(body, "room_code");
            std::string uid = StringUtils::jsonVal(body, "uid");
            if (code.empty())
                return AuthService::jsonErr("FIELDS_REQUIRED", "需要 room_code");
            if (uid.empty())
                return AuthService::jsonErr("FIELDS_REQUIRED", "需要 uid");

            auto &r = RedisClient::instance();
            std::string k0 = "peer:" + code + ":0";
            std::string k1 = "peer:" + code + ":1";

            auto v0 = r.get(k0);
            auto v1 = r.get(k1);

            // 按 uid 定位自身槽位
            int mySlot = -1;
            {
                auto uid0 = r.get("peer:" + code + ":uid0");
                auto uid1 = r.get("peer:" + code + ":uid1");
                if (uid0 && *uid0 == uid && v0) mySlot = 0;
                else if (uid1 && *uid1 == uid && v1) mySlot = 1;
            }

            bool isController = false;
            if (mySlot == 0) {
                isController = true;
                r.del(k0);
                r.del("peer:" + code + ":uid0");
                r.del("peer:" + code + ":email");
                // 控制器退出 → 关闭房间，通知被控制方
                r.setex("peer:" + code + ":notify", NOTIFY_TTL, "room_closed");
                if (v1) r.del(k1);
                if (v1) r.del("peer:" + code + ":uid1");
            } else if (mySlot == 1) {
                r.del(k1);
                r.del("peer:" + code + ":uid1");
                // 被控制方退出 → 通知控制方
                r.setex("peer:" + code + ":notify", NOTIFY_TTL, "peer_left");
            }

            // 清理中转队列
            r.del("relay:" + code + ":0");
            r.del("relay:" + code + ":1");
            r.del("relay:" + code + ":seq");

            // 清理用户→房间映射
            r.del("peer:user:" + tp->email);

            if (mySlot < 0)
                return AuthService::jsonOk("ok");   // 不在房间也幂等返回成功
            return AuthService::jsonOk(isController ? "房间已关闭" : "已退出房间");
        });

    // ═══════════════════════════════════════════════════════
    //  POST /api/peer/heartbeat
    //  续期自身槽位（兼容保留；活跃续期已由 peers/push/pull 覆盖）
    //  Body: {room_code: "ABC123", uid: "设备唯一标识"}
    //  ═══════════════════════════════════════════════════════
    server.route("POST", "/api/peer/heartbeat",
        [&](const std::string &body, const std::string &, const std::string &headers) {
            auto tp = auth.authenticate(headers);
            if (!tp) return AuthService::jsonErr("UNAUTHORIZED", "请先登录");

            std::string code = StringUtils::jsonVal(body, "room_code");
            std::string uid = StringUtils::jsonVal(body, "uid");
            if (code.empty() || uid.empty())
                return AuthService::jsonErr("FIELDS_REQUIRED", "需要 room_code 和 uid");

            auto &r = RedisClient::instance();

            // 按 uid 定位自身槽位并续期
            auto v0 = r.get("peer:" + code + ":0");
            auto v1 = r.get("peer:" + code + ":1");
            int mySlot = -1;
            {
                auto uid0 = r.get("peer:" + code + ":uid0");
                auto uid1 = r.get("peer:" + code + ":uid1");
                if (uid0 && *uid0 == uid && v0) mySlot = 0;
                else if (uid1 && *uid1 == uid && v1) mySlot = 1;
            }
            if (mySlot < 0)
                return AuthService::jsonErr("NOT_FOUND", "你的设备不在房间中，请重新加入");

            r.expire("peer:" + code + ":" + std::to_string(mySlot), ROOM_TTL);
            r.expire("peer:" + code + ":uid" + std::to_string(mySlot), ROOM_TTL);
            if (mySlot == 0) r.expire("peer:" + code + ":email", ROOM_TTL);

            int count = 0;
            if (v0) count++;
            if (v1) count++;

            std::ostringstream resp;
            resp << "{\"success\":true,\"member_count\":" << count << ",\"member_max\":2}";
            return resp.str();
        });
}
