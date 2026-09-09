#include "PeerStore.h"
#include "../core/cache/RedisClient.h"
#include "../common/StringUtils.h"
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <iterator>
#include <sstream>

// ═══════════════════════════════════════════════════════════════
//  常量与键工具 (2026-08-14 v2 原实现, 逐字迁移自 PeerRoutes)
// ═══════════════════════════════════════════════════════════════
static const int  ROOM_TTL   = 120;  // 房间槽位 TTL（秒）
static const int  RELAY_TTL  = 300;  // 中转队列/序号 TTL（秒）
static const int  NOTIFY_TTL = 30;   // 离开通知 TTL
static const long RELAY_MAX  = 100;  // 每队列最多保留条数
static const char RC_CHARS[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
static const int  RC_LEN = sizeof(RC_CHARS) - 1;

// ── 6位房间码生成 (2026-09-09: rand() 换 mt19937_64, 房间码不再可预测) ──
static std::string generateRoomCode() {
    static std::mt19937_64 rng(
        std::random_device{}() ^ (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count());
    std::string code(6, '0');
    for (int i = 0; i < 6; ++i)
        code[i] = RC_CHARS[rng() % RC_LEN];
    return code;
}

// ── 从 "ip:port" 字符串解析 ──
static bool parseAddr(const std::string &val, std::string &ip, int &port) {
    auto colon = val.find(':');
    if (colon == std::string::npos) return false;
    ip = val.substr(0, colon);
    try { port = std::stoi(val.substr(colon + 1)); }
    catch (...) { return false; }
    return port > 0;
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

// ═══════════════════════════════════════════════════════════════
//  createRoom
// ═══════════════════════════════════════════════════════════════
PeerStore::CreateResult PeerStore::createRoom(const std::string &email,
                                              const std::string &addr,
                                              const std::string &uid,
                                              const std::string &roomName) {
    CreateResult out;
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

    // 生成唯一房间码 (SETNX 防碰撞, 最多重试 20 次)
    std::string code;
    for (int retry = 0; retry < 20; ++retry) {
        code = generateRoomCode();
        if (r.setnx("peer:" + code + ":lock", "1")) {
            r.expire("peer:" + code + ":lock", 5);
            break;
        }
        code.clear();
    }
    if (code.empty()) {
        out.errcode = "ROOM_CREATE_FAILED"; out.msg = "生成房间码失败，请重试";
        return out;
    }

    // 写 slot 0（控制方）
    r.setex("peer:" + code + ":0", ROOM_TTL, addr);
    r.setex("peer:" + code + ":uid0", ROOM_TTL, uid);
    r.setex("peer:" + code + ":email", ROOM_TTL, email);
    r.setex("peer:user:" + email, ROOM_TTL, code);
    if (!roomName.empty())          // sync 客户端房间名 (2026-09-10)
        r.setex("peer:" + code + ":name", ROOM_TTL, roomName);

    out.ok = true;
    out.roomCode = code;
    return out;
}

// ═══════════════════════════════════════════════════════════════
//  joinRoom
// ═══════════════════════════════════════════════════════════════
PeerStore::JoinResult PeerStore::joinRoom(const std::string &code,
                                          const std::string &addr,
                                          const std::string &uid) {
    JoinResult out;
    auto &r = RedisClient::instance();

    // 检查房间是否存在
    auto v0 = r.get("peer:" + code + ":0");
    if (!v0) {
        out.errcode = "ROOM_NOT_FOUND"; out.msg = "房间不存在或已过期";
        return out;
    }

    // 同一设备不能加入自己创建的房间 (uid 相同; 同账号不同设备允许)
    auto uid0 = r.get("peer:" + code + ":uid0");
    if (uid0 && *uid0 == uid) {
        out.errcode = "SELF_JOIN"; out.msg = "同一设备不能加入自己创建的房间";
        return out;
    }

    // 检查是否已有其他设备占用了被控制方槽位
    auto v1 = r.get("peer:" + code + ":1");
    if (v1) {
        auto uid1 = r.get("peer:" + code + ":uid1");
        // 自己重连 (同 uid) → 更新 TTL
        if (uid1 && *uid1 == uid) {
            r.setex("peer:" + code + ":1", ROOM_TTL, addr);
            r.setex("peer:" + code + ":uid1", ROOM_TTL, uid);
            r.expire("peer:" + code + ":0", ROOM_TTL);
            r.expire("peer:" + code + ":uid0", ROOM_TTL);
            std::string peerIp; int peerPort;
            if (parseAddr(*v0, peerIp, peerPort)) {
                out.ok = true; out.reconnect = true;
                out.peer = Peer{peerIp, peerPort};
                return out;
            }
        }
        out.errcode = "ROOM_FULL"; out.msg = "房间已满";
        return out;
    }

    // 写入 slot 1（被控制方）
    r.setex("peer:" + code + ":1", ROOM_TTL, addr);
    r.setex("peer:" + code + ":uid1", ROOM_TTL, uid);
    // 续期 slot 0
    r.expire("peer:" + code + ":0", ROOM_TTL);
    r.expire("peer:" + code + ":uid0", ROOM_TTL);
    r.expire("peer:" + code + ":email", ROOM_TTL);

    // 返回对方（控制方）地址
    std::string peerIp; int peerPort;
    if (!parseAddr(*v0, peerIp, peerPort)) {
        out.errcode = "INVALID"; out.msg = "控制方数据损坏";
        return out;
    }

    out.ok = true;
    out.peer = Peer{peerIp, peerPort};
    return out;
}

// ═══════════════════════════════════════════════════════════════
//  roomStatus
// ═══════════════════════════════════════════════════════════════
PeerStore::StatusResult PeerStore::roomStatus(const std::string &code) {
    StatusResult out;
    auto &r = RedisClient::instance();

    auto v0 = r.get("peer:" + code + ":0");
    auto v1 = r.get("peer:" + code + ":1");

    int count = 0;
    if (v0) {
        count++;
        std::string ip; int port;
        if (parseAddr(*v0, ip, port)) out.controller = Peer{ip, port};
        auto u0 = r.get("peer:" + code + ":uid0");
        if (u0) out.controllerUid = *u0;
    }
    if (v1) {
        count++;
        std::string ip; int port;
        if (parseAddr(*v1, ip, port)) out.controlled = Peer{ip, port};
        auto u1 = r.get("peer:" + code + ":uid1");
        if (u1) out.controlledUid = *u1;
    }

    // 检查离开通知 (一次性消费)
    auto notify = r.get("peer:" + code + ":notify");
    if (notify) {
        out.notify = *notify;
        r.del("peer:" + code + ":notify");
    }

    // 房间名 (sync 客户端, 2026-09-10)
    auto nm = r.get("peer:" + code + ":name");
    if (nm) out.roomName = *nm;

    out.ok = true;
    out.memberCount = count;
    return out;
}

// ═══════════════════════════════════════════════════════════════
//  getPeers — 取对端地址 + 续期自身槽位
// ═══════════════════════════════════════════════════════════════
PeerStore::PeersResult PeerStore::getPeers(const std::string &code,
                                           const std::string &uid) {
    PeersResult out;
    auto &r = RedisClient::instance();

    // 检查对方是否离开了 (消费一次性通知)
    auto notify = r.get("peer:" + code + ":notify");
    if (notify) {
        std::string msg = *notify;
        r.del("peer:" + code + ":notify");
        if (msg == "peer_left") {
            out.ok = true; out.specialStatus = "peer_left"; out.memberCount = 1;
            return out;
        }
        if (msg == "room_closed") {
            out.ok = true; out.specialStatus = "room_closed"; out.memberCount = 0;
            return out;
        }
    }

    auto v0 = r.get("peer:" + code + ":0");
    auto v1 = r.get("peer:" + code + ":1");

    if (!v0 && !v1) {
        out.errcode = "ROOM_NOT_FOUND"; out.msg = "房间不存在或已过期";
        return out;
    }

    // 按 uid 定位自己所在的槽位 (0=控制方, 1=被控制方)
    int mySlot = -1;
    {
        auto uid0 = r.get("peer:" + code + ":uid0");
        auto uid1 = r.get("peer:" + code + ":uid1");
        if (uid0 && *uid0 == uid && v0) mySlot = 0;
        else if (uid1 && *uid1 == uid && v1) mySlot = 1;
    }
    if (mySlot < 0) {
        out.errcode = "NOT_FOUND"; out.msg = "你的设备不在房间中，请重新加入";
        return out;
    }

    // 续期自己所在的槽位
    if (mySlot == 0) {
        r.expire("peer:" + code + ":0", ROOM_TTL);
        r.expire("peer:" + code + ":uid0", ROOM_TTL);
        r.expire("peer:" + code + ":email", ROOM_TTL);
    } else {
        r.expire("peer:" + code + ":1", ROOM_TTL);
        r.expire("peer:" + code + ":uid1", ROOM_TTL);
    }

    // 返回另一个槽位; 对方未加入 → peer 空 (轮询静默等待)
    auto other = (mySlot == 0) ? v1 : v0;
    int count = 0;
    if (v0) count++;
    if (v1) count++;
    out.memberCount = count;

    if (!other) { out.ok = true; return out; }

    std::string peerIp; int peerPort;
    if (!parseAddr(*other, peerIp, peerPort)) {
        out.errcode = "INVALID"; out.msg = "数据损坏";
        return out;
    }
    out.ok = true;
    out.peer = Peer{peerIp, peerPort};
    return out;
}

// ═══════════════════════════════════════════════════════════════
//  push — 数据进对端 relay 队列
// ═══════════════════════════════════════════════════════════════
PeerStore::PushResult PeerStore::push(const std::string &code,
                                      const std::string &uid,
                                      const std::string &op,
                                      double x, double y,
                                      const std::string &extraJson) {
    PushResult out;
    auto &r = RedisClient::instance();

    auto v0 = r.get("peer:" + code + ":0");
    auto v1 = r.get("peer:" + code + ":1");
    if (!v0 && !v1) {
        out.errcode = "ROOM_NOT_FOUND"; out.msg = "房间不存在或已过期";
        return out;
    }

    // 定位发送方槽位 → 数据进对端队列
    int mySlot = -1;
    {
        auto uid0 = r.get("peer:" + code + ":uid0");
        auto uid1 = r.get("peer:" + code + ":uid1");
        if (uid0 && *uid0 == uid && v0) mySlot = 0;
        else if (uid1 && *uid1 == uid && v1) mySlot = 1;
    }
    if (mySlot < 0) {
        out.errcode = "NOT_FOUND"; out.msg = "你的设备不在房间中，请重新加入";
        return out;
    }

    // 续期自身槽位 (活跃即保活)
    r.expire("peer:" + code + ":" + std::to_string(mySlot), ROOM_TTL);
    r.expire("peer:" + code + ":uid" + std::to_string(mySlot), ROOM_TTL);
    if (mySlot == 0) r.expire("peer:" + code + ":email", ROOM_TTL);

    // 对端不在线 → 数据直接丢弃 (操作坐标是瞬态数据)
    int dstSlot = (mySlot == 0) ? 1 : 0;
    auto dst = (dstSlot == 0) ? v0 : v1;
    if (!dst) {
        out.errcode = "PEER_LEFT"; out.msg = "对方已退出房间";
        return out;
    }

    // 全局序号
    long long seq = r.incr("relay:" + code + ":seq");
    r.expire("relay:" + code + ":seq", RELAY_TTL);

    std::ostringstream item;
    item << "{\"seq\":" << seq
         << ",\"op\":\"" << StringUtils::jsonEscape(op) << "\""
         << ",\"x\":" << x
         << ",\"y\":" << y
         << extraJson
         << "}";

    std::string listKey = "relay:" + code + ":" + std::to_string(dstSlot);
    r.rpush(listKey, item.str());
    r.ltrim(listKey, -RELAY_MAX, -1);   // 只留最近 100 条
    r.expire(listKey, RELAY_TTL);

    out.ok = true;
    return out;
}

// ═══════════════════════════════════════════════════════════════
//  pull — 长轮询拉取
// ═══════════════════════════════════════════════════════════════
PeerStore::PullResult PeerStore::pull(const std::string &code,
                                      const std::string &uid,
                                      long long lastSeq, int waitMs) {
    PullResult out;
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
    if (mySlot < 0) { out.roomClosed = true; return out; }

    std::string listKey = "relay:" + code + ":" + std::to_string(mySlot);

    // 长轮询: 最多等 waitMs
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(waitMs);
    while (true) {
        std::string item;
        if (relayPopNext(r, listKey, lastSeq, item)) {
            out.ok = true; out.hasData = true; out.data = item;
            return out;
        }
        // 对端槽位消失 → 对方已退出 (我还在房间)
        if (!r.get("peer:" + code + ":" + std::to_string(1 - mySlot))) {
            out.peerLeft = true; return out;
        }
        // 我方槽位消失 → 房间整体关闭
        if (!r.get("peer:" + code + ":" + std::to_string(mySlot))) {
            out.roomClosed = true; return out;
        }
        if (std::chrono::steady_clock::now() >= deadline) break;
        // 步进自适应 (2026-09-10): 长轮询 (>2s) 用 50ms 步进控制 Redis 查询量,
        // 短轮询保持 10ms 低延迟 (peer 语义)
        std::this_thread::sleep_for(std::chrono::milliseconds(waitMs > 2000 ? 50 : 10));
    }

    // 无数据也要续期 (轮询即活跃)
    r.expire("peer:" + code + ":" + std::to_string(mySlot), ROOM_TTL);
    r.expire("peer:" + code + ":uid" + std::to_string(mySlot), ROOM_TTL);
    if (mySlot == 0) r.expire("peer:" + code + ":email", ROOM_TTL);

    out.ok = true;
    return out;
}

// ═══════════════════════════════════════════════════════════════
//  unregister — 退出 / 关房
// ═══════════════════════════════════════════════════════════════
PeerStore::UnregisterResult PeerStore::unregister(const std::string &code,
                                                  const std::string &uid,
                                                  const std::string &email) {
    UnregisterResult out;
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

    if (mySlot == 0) {
        out.closedRoom = true;
        out.wasInRoom = true;
        r.del(k0);
        r.del("peer:" + code + ":uid0");
        r.del("peer:" + code + ":email");
        // 控制器退出 → 关闭房间，通知被控制方
        r.setex("peer:" + code + ":notify", NOTIFY_TTL, "room_closed");
        if (v1) r.del(k1);
        if (v1) r.del("peer:" + code + ":uid1");
    } else if (mySlot == 1) {
        out.wasInRoom = true;
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
    r.del("peer:user:" + email);

    out.ok = true;   // 不在房间也幂等返回成功
    return out;
}

// ═══════════════════════════════════════════════════════════════
//  heartbeat — 续期 (兼容保留)
// ═══════════════════════════════════════════════════════════════
PeerStore::HeartbeatResult PeerStore::heartbeat(const std::string &code,
                                                const std::string &uid) {
    HeartbeatResult out;
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
    if (mySlot < 0) {
        out.errcode = "NOT_FOUND"; out.msg = "你的设备不在房间中，请重新加入";
        return out;
    }

    r.expire("peer:" + code + ":" + std::to_string(mySlot), ROOM_TTL);
    r.expire("peer:" + code + ":uid" + std::to_string(mySlot), ROOM_TTL);
    if (mySlot == 0) r.expire("peer:" + code + ":email", ROOM_TTL);

    int count = 0;
    if (v0) count++;
    if (v1) count++;

    out.ok = true;
    out.memberCount = count;
    return out;
}
