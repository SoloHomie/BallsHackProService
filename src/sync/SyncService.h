#pragma once

#include "../core/database/DbPool.h"
#include <string>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <unordered_map>
#include <memory>
#include <cstdint>
#include <chrono>

// ── 坐标快照 ──
struct SyncCommand {
    std::string opType;
    std::string extra;    // JSON: {"delay_ms":N}
    double      x = 0.0;
    double      y = 0.0;
    int64_t     ts = 0;   // unix ms
};

// ── 每房间的同步状态（条件变量通知）──
struct RoomSyncState {
    SyncCommand cmd;
    uint64_t    seq   = 0;
    bool        dirty = false;
    std::mutex  mtx;
    std::condition_variable cv;

    // 房间成员（用于 push/pull 授权检查）
    std::string controllerEmail;
    std::string controlledEmail;

    // 最后活跃时间（用于清理僵尸房间）
    std::chrono::steady_clock::time_point lastActive;

    // 房间关闭标记（创建者退出时设为 true，拉取方可感知）
    bool roomClosed = false;

    // 等待新数据，timeoutMs 超时返回 false
    bool waitForNew(uint64_t lastSeq, int timeoutMs);

    void touch() { lastActive = std::chrono::steady_clock::now(); }
};

// SyncService — 房间管理（MySQL）+ 坐标中转（内存 + 条件变量通知）
class SyncService {
public:
    explicit SyncService(DbPool &pool) : m_pool(pool) {}

    // ── 房间操作 ──
    std::string createRoom(const std::string &roomName, const std::string &email);
    std::string joinRoom(const std::string &roomCode, const std::string &email);
    std::string leaveRoom(const std::string &roomCode, const std::string &email);
    std::string roomStatus(const std::string &roomCode);

    // ── 坐标中转 ──
    // 控制方推送（通知等待中的受控制方）
    std::string pushCommand(const std::string &roomCode, const std::string &email,
                            const std::string &opType, double x, double y,
                            const std::string &extra = "");
    // 受控制方拉取（阻塞等待，有数据或超时才返回）
    std::string pullCommand(const std::string &roomCode, const std::string &email,
                            uint64_t lastSeq, int waitMs);

    // 清理僵尸房间（超过 idleSeconds 未活动）
    void purgeIdleRooms(int idleSeconds = 600);
    // 关闭房间（清理 MySQL + 内存状态）
    void closeRoom(const std::string &roomCode);
    // 服务重启后恢复房间内存状态
    void recoverFromDb();

    // 获取或创建房间状态（供清理循环同步内存状态）
    std::shared_ptr<RoomSyncState> getOrCreateState(const std::string &roomCode);
    // 只读查找（不创建），用于避免无效请求污染 m_rooms
    std::shared_ptr<RoomSyncState> findState(const std::string &roomCode);

private:
    DbPool &m_pool;

    // ── 房间状态表 ──
    std::shared_mutex m_roomsMutex;
    std::unordered_map<std::string, std::shared_ptr<RoomSyncState>> m_rooms;

    void removeState(const std::string &roomCode, bool notifyClosed = false);

    static std::string generateRoomCode();
    static std::string serializeCommand(const SyncCommand &cmd, uint64_t seq);
};
