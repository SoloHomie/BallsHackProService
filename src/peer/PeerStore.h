#pragma once
// ═══════════════════════════════════════════════════════════════
//  PeerStore — 远程控制房间的 Redis 存储层 (2026-09-09 收敛自 PeerRoutes)
//  2026-09-10 合并: /api/sync/* (原 SyncService MySQL+内存) 也走本键族,
//    SyncRoutes 以 uid=email 定位调用 (见 docs/sync-peer.md 行为差异清单)。
// ═══════════════════════════════════════════════════════════════
// 键设计 (集中管理, 业务方不得直接拼键):
//   peer:<code>:0      → 控制方 public_ip:port (sync 语义下为空串)
//   peer:<code>:uid0   → 控制方设备 uid (sync 语义下=email)
//   peer:<code>:1      → 被控制方 public_ip:port (sync 语义下为空串)
//   peer:<code>:uid1   → 被控制方设备 uid (sync 语义下=email)
//   peer:<code>:email  → 控制方 email (旧房间清理用)
//   peer:<code>:name   → 房间名 (sync 客户端传入, peer 不使用)
//   peer:<code>:notify → 离开/关房通知 (peer_left / room_closed, TTL 30s)
//   peer:<code>:lock   → 建码防撞锁 (SETNX, 5s)
//   peer:user:<email>  → 用户 → 房间码 映射
//   relay:<code>:0/1   → 中转数据队列 (按目标槽位分列, 上限 100 条)
//   relay:<code>:seq   → 全局数据序号
// 规则: 槽位按设备 uid 定位 (非 ip:port); 同账号不同设备可互连,
//   同一设备 (uid 相同) 禁止自加入。TTL: 房间 120s 活跃续期。
#include <string>
#include <optional>
#include <cstdint>

class PeerStore {
public:
    struct Peer { std::string ip; int port = 0; };

    // 创建房间: 清旧房间 → 生成唯一 6 位房码 → 写 slot0 (控制方)
    // roomName: sync 客户端传入 (存 peer:<code>:name), peer 传空串
    struct CreateResult {
        bool ok = false; std::string errcode, msg;
        std::string roomCode;
    };
    CreateResult createRoom(const std::string &email, const std::string &addr,
                            const std::string &uid, const std::string &roomName = "");

    // 加入房间: 存在性/自加入/占用检查 → 写 slot1 → 返回控制方地址
    struct JoinResult {
        bool ok = false; std::string errcode, msg;
        bool reconnect = false;             // 同设备重连 (更新 TTL)
        std::optional<Peer> peer;           // 控制方地址
    };
    JoinResult joinRoom(const std::string &code, const std::string &addr,
                        const std::string &uid);

    // 房间状态: 成员数 + 双方地址 + 消费离开通知 + 房间名 + 槽位原始 uid
    // (sync 语义下 uid0/uid1 即邮箱, 供 controller_email/controlled_email 回显)
    struct StatusResult {
        bool ok = false; std::string errcode, msg;
        int memberCount = 0;
        std::string roomName;
        std::string controllerUid, controlledUid;
        std::optional<Peer> controller, controlled;
        std::string notify;                 // 消费掉的离开通知 (一次性)
    };
    StatusResult roomStatus(const std::string &code);

    // 取对端地址 + 续期自身槽位 (轮询即活跃)
    struct PeersResult {
        bool ok = false; std::string errcode, msg;
        std::string specialStatus;          // 非空 = 特殊响应: peer_left / room_closed
        int memberCount = 0;
        std::optional<Peer> peer;           // 空 = 对方未加入 (静默等待)
    };
    PeersResult getPeers(const std::string &code, const std::string &uid);

    // 推送数据进对端 relay 队列 (带序号, 只留最近 100 条)
    // extraJson: 追加到 item 的字段, 形如 ",\"split\":true" (peer) 或
    //   ",\"ts\":123,"extra":{...}" (sync, 2026-09-10 合并适配)
    struct PushResult { bool ok = false; std::string errcode, msg; };
    PushResult push(const std::string &code, const std::string &uid,
                    const std::string &op, double x, double y,
                    const std::string &extraJson);

    // 长轮询拉取: 最多等 waitMs, 有数据即返; 对端/我方消失 → peerLeft/roomClosed
    struct PullResult {
        bool ok = false; std::string errcode, msg;
        bool hasData = false; std::string data;
        bool peerLeft = false;
        bool roomClosed = false;
    };
    PullResult pull(const std::string &code, const std::string &uid,
                    long long lastSeq, int waitMs);

    // 退出: 控制方退出=关房; 被控方退出=通知对方。不在房间幂等成功。
    struct UnregisterResult {
        bool ok = false; std::string errcode, msg;
        bool closedRoom = false;   // 控制方退出 → 关房
        bool wasInRoom = false;    // false = 不在房间 (幂等, 响应 "ok")
    };
    UnregisterResult unregister(const std::string &code, const std::string &uid,
                                const std::string &email);

    // 续期自身槽位 (兼容保留; 活跃续期已由 peers/push/pull 覆盖)
    struct HeartbeatResult {
        bool ok = false; std::string errcode, msg;
        int memberCount = 0;
    };
    HeartbeatResult heartbeat(const std::string &code, const std::string &uid);
};
