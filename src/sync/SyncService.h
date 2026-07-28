#pragma once

#include "../core/database/DbPool.h"
#include <string>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <unordered_map>
#include <memory>
#include <cstdint>

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

    // 等待新数据，timeoutMs 超时返回 false
    bool waitForNew(uint64_t lastSeq, int timeoutMs);
};

// SyncService — 房间管理（MySQL）+ 坐标中转（内存 + 条件变量通知）
class SyncService {
public:
    explicit SyncService(DbPool &pool) : m_pool(pool) {}

    // ── 房间操作 ──
    std::string createRoom(const std::string &roomName, const std::string &email);
    std::string joinRoom(const std::string &roomCode, const std::string &email,
                         const std::string &role);
    std::string leaveRoom(const std::string &roomCode, const std::string &email);
    std::string roomStatus(const std::string &roomCode);

    // ── 坐标中转 ──
    // 控制方推送（通知等待中的受控制方）
    void        pushCommand(const std::string &roomCode, const std::string &opType,
                            double x, double y, const std::string &extra = "");
    // 受控制方拉取（阻塞等待，有数据或超时才返回）
    // lastSeq=0 首次拉取，waitMs=5000 最长等待 5s
    std::string pullCommand(const std::string &roomCode, uint64_t lastSeq, int waitMs);

private:
    DbPool &m_pool;

    // ── 房间状态表 ──
    std::shared_mutex m_roomsMutex;
    std::unordered_map<std::string, std::shared_ptr<RoomSyncState>> m_rooms;

    std::shared_ptr<RoomSyncState> getOrCreateState(const std::string &roomCode);
    void removeState(const std::string &roomCode);

    static std::string generateRoomCode();
    static std::string serializeCommand(const SyncCommand &cmd, uint64_t seq);
};
