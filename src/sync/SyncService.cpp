#include "SyncService.h"
#include "../common/Snowflake.h"
#include "../common/Logger.h"
#include "../core/cache/RedisClient.h"
#include <mysql/mysql.h>
#include <sstream>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <cstring>

// SQL escape helper
static std::string esc(MYSQL *db, const std::string &s) {
    if (s.empty()) return s;
    char *buf = new char[s.size() * 2 + 1];
    mysql_real_escape_string(db, buf, s.c_str(), (unsigned long)s.size());
    std::string r(buf);
    delete[] buf;
    return r;
}

// ═══════════════════════════════════════════════════════════
//  RoomSyncState — 条件变量等待
// ═══════════════════════════════════════════════════════════

bool RoomSyncState::waitForNew(uint64_t lastSeq, int timeoutMs) {
    std::unique_lock<std::mutex> lk(mtx);
    return cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&]() {
        return dirty && seq > lastSeq;
    });
}

// ═══════════════════════════════════════════════════════════
//  房间码生成
// ═══════════════════════════════════════════════════════════

static const char RC_CHARS[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
static const int  RC_LEN     = sizeof(RC_CHARS) - 1;

std::string SyncService::generateRoomCode() {
    std::string code(6, '0');
    for (int i = 0; i < 6; ++i)
        code[i] = RC_CHARS[rand() % RC_LEN];
    return code;
}

// ═══════════════════════════════════════════════════════════
//  房间状态表管理
// ═══════════════════════════════════════════════════════════

std::shared_ptr<RoomSyncState> SyncService::getOrCreateState(const std::string &roomCode) {
    {
        std::shared_lock<std::shared_mutex> lk(m_roomsMutex);
        auto it = m_rooms.find(roomCode);
        if (it != m_rooms.end()) {
            it->second->touch();
            return it->second;
        }
    }
    {
        std::unique_lock<std::shared_mutex> lk(m_roomsMutex);
        auto it = m_rooms.find(roomCode);
        if (it != m_rooms.end()) {
            it->second->touch();
            return it->second;
        }
        auto state = std::make_shared<RoomSyncState>();
        state->touch();
        m_rooms[roomCode] = state;
        return state;
    }
}

std::shared_ptr<RoomSyncState> SyncService::findState(const std::string &roomCode) {
    std::shared_lock<std::shared_mutex> lk(m_roomsMutex);
    auto it = m_rooms.find(roomCode);
    return (it != m_rooms.end()) ? it->second : nullptr;
}

void SyncService::removeState(const std::string &roomCode, bool notifyClosed) {
    std::unique_lock<std::shared_mutex> lk(m_roomsMutex);
    auto it = m_rooms.find(roomCode);
    if (it != m_rooms.end()) {
        {
            std::lock_guard<std::mutex> rlk(it->second->mtx);
            it->second->dirty = false;
            if (notifyClosed)
                it->second->roomClosed = true;
        }
        it->second->cv.notify_all();
        m_rooms.erase(it);
    }
}

// ── 服务重启后恢复房间内存状态 ──
void SyncService::recoverFromDb() {
    auto guard = m_pool.acquire(5000);
    if (!guard) return;
    MYSQL *db = guard->get();
    if (mysql_query(db, "SELECT room_code, controller_email, controlled_email FROM rooms WHERE status != 2"))
        return;
    MYSQL_RES *res = mysql_store_result(db);
    if (!res) return;
    MYSQL_ROW row;
    int recovered = 0;
    while ((row = mysql_fetch_row(res))) {
        std::string code(row[0] ? row[0] : "");
        std::string ctrl(row[1] ? row[1] : "");
        std::string ctrld(row[2] ? row[2] : "");
        if (code.empty()) continue;
        auto state = getOrCreateState(code);
        state->controllerEmail = ctrl;
        state->controlledEmail = ctrld;
        recovered++;
    }
    mysql_free_result(res);
    std::cout << "[Sync] Recovered " << recovered << " rooms from DB" << std::endl;
}

void SyncService::closeRoom(const std::string &roomCode) {
    // 清理 Redis 缓存（在 removeState 之前，因为需要读取成员邮箱）
    {
        std::shared_lock<std::shared_mutex> lk(m_roomsMutex);
        auto it = m_rooms.find(roomCode);
        if (it != m_rooms.end()) {
            try {
                if (!it->second->controllerEmail.empty())
                    RedisClient::instance().del("user_room:" + it->second->controllerEmail);
                if (!it->second->controlledEmail.empty())
                    RedisClient::instance().del("user_room:" + it->second->controlledEmail);
            } catch (...) {}
        }
    }

    auto guard = m_pool.acquire(5000);
    if (guard) {
        MYSQL *db = guard->get();
        char q[256];
        snprintf(q, sizeof(q), "UPDATE rooms SET status=2 WHERE room_code='%s'",
                 esc(db, roomCode).c_str());
        mysql_query(db, q);
    }
    removeState(roomCode, true);
}

void SyncService::purgeIdleRooms(int idleSeconds) {
    auto now = std::chrono::steady_clock::now();
    std::unique_lock<std::shared_mutex> lk(m_roomsMutex);
    auto it = m_rooms.begin();
    while (it != m_rooms.end()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - it->second->lastActive).count();
        if (elapsed > idleSeconds) {
            {
                std::lock_guard<std::mutex> rlk(it->second->mtx);
                it->second->dirty = false;
            }
            it->second->cv.notify_all();
            it = m_rooms.erase(it);
        } else {
            ++it;
        }
    }
}

std::string SyncService::serializeCommand(const SyncCommand &cmd, uint64_t seq) {
    std::ostringstream s;
    s << "{\"op\":\"" << cmd.opType << "\""
      << ",\"x\":" << cmd.x
      << ",\"y\":" << cmd.y
      << ",\"ts\":" << cmd.ts
      << ",\"seq\":" << seq;
    if (!cmd.extra.empty())
        s << ",\"extra\":" << cmd.extra;
    s << "}";
    return s.str();
}

// ═══════════════════════════════════════════════════════════
//  房间操作（MySQL）
// ═══════════════════════════════════════════════════════════

std::string SyncService::createRoom(const std::string &roomName, const std::string &email) {
    if (email.empty())
        return "{\"success\":false,\"errcode\":\"FIELDS_REQUIRED\",\"message\":\"邮箱不能为空\"}";

    // ── 1. 先查 Redis 缓存 ──
    try {
        auto opt = RedisClient::instance().get("user_room:" + email);
        if (opt) {
            std::string cachedCode = *opt;
            auto guard = m_pool.acquire(5000);
            if (guard) {
                MYSQL *db = guard->get();
                char cq[512];
                snprintf(cq, sizeof(cq),
                    "SELECT room_code,room_name,controller_email,controlled_email,status "
                    "FROM rooms WHERE room_code='%s' AND status != 2",
                    esc(db, cachedCode).c_str());
                if (mysql_query(db, cq) == 0) {
                    MYSQL_RES *res = mysql_store_result(db);
                    if (res && mysql_num_rows(res) > 0) {
                        MYSQL_ROW row = mysql_fetch_row(res);
                        std::string code = row[0] ? row[0] : "";
                        std::string name = row[1] ? row[1] : "";
                        std::string ctrl = row[3] ? row[3] : "";
                        std::string ctrld = row[4] ? row[4] : "";
                        int mc = 1 + (ctrld.empty() ? 0 : 1);
                        mysql_free_result(res);
                        // 确保内存状态存在
                        auto state = getOrCreateState(code);
                        if (state->controllerEmail.empty()) state->controllerEmail = ctrl;
                        if (state->controlledEmail.empty()) state->controlledEmail = ctrld;
                        std::ostringstream resp;
                        resp << "{\"success\":true"
                             << ",\"already_exists\":true"
                             << ",\"room_code\":\"" << code << "\""
                             << ",\"room_name\":\"" << name << "\""
                             << ",\"your_role\":\"controller\""
                             << ",\"member_count\":" << mc << ",\"member_max\":2"
                             << "}";
                        return resp.str();
                    }
                    if (res) mysql_free_result(res);
                }
            }
            // Redis 缓存过期/脏数据，删掉后继续查 MySQL
            try { RedisClient::instance().del("user_room:" + email); } catch (...) {}
        }
    } catch (...) {}

    auto guard = m_pool.acquire(5000);
    if (!guard)
        return "{\"success\":false,\"errcode\":\"DB_ERROR\",\"message\":\"数据库繁忙\"}";
    MYSQL *db = guard->get();

    // ── 2. 查 MySQL 是否有活跃房间 → 有则返回已有房间 ──
    {
        std::string eEmail = esc(db, email);
        char cq[512];
        snprintf(cq, sizeof(cq),
            "SELECT room_code,room_name,controller_email,controlled_email,status "
            "FROM rooms WHERE (controller_email='%s' OR controlled_email='%s') AND status != 2 LIMIT 1",
            eEmail.c_str(), eEmail.c_str());
        if (mysql_query(db, cq) == 0) {
            MYSQL_RES *res = mysql_store_result(db);
            if (res && mysql_num_rows(res) > 0) {
                MYSQL_ROW row = mysql_fetch_row(res);
                std::string code = row[0] ? row[0] : "";
                std::string name = row[1] ? row[1] : "";
                std::string ctrl = row[3] ? row[3] : "";
                std::string ctrld = row[4] ? row[4] : "";
                int mc = 1 + (ctrld.empty() ? 0 : 1);
                mysql_free_result(res);
                // 回种 Redis
                try { RedisClient::instance().setex("user_room:" + email, 3600, code); } catch (...) {}
                // 确保内存状态存在
                auto state = getOrCreateState(code);
                if (state->controllerEmail.empty()) state->controllerEmail = ctrl;
                if (state->controlledEmail.empty()) state->controlledEmail = ctrld;
                bool isCtrl = (email == ctrl);
                std::ostringstream resp;
                resp << "{\"success\":true"
                     << ",\"already_exists\":true"
                     << ",\"room_code\":\"" << code << "\""
                     << ",\"room_name\":\"" << name << "\""
                     << ",\"your_role\":\"" << (isCtrl ? "controller" : "controlled") << "\""
                     << ",\"member_count\":" << mc << ",\"member_max\":2"
                     << "}";
                Logger::info("SyncService", std::string("Room ") + code + " already exists for " + email + ", returned existing");
                return resp.str();
            }
            if (res) mysql_free_result(res);
        }
    }

    // ── 3. 生成唯一 room code ──
    std::string code;
    for (int retry = 0; retry < 10; ++retry) {
        code = generateRoomCode();
        char q[256];
        snprintf(q, sizeof(q),
                 "SELECT COUNT(*) FROM rooms WHERE room_code='%s'", esc(db, code).c_str());
        if (mysql_query(db, q) == 0) {
            MYSQL_RES *res = mysql_store_result(db);
            if (res) {
                MYSQL_ROW row = mysql_fetch_row(res);
                bool unique = (row && atoi(row[0]) == 0);
                mysql_free_result(res);
                if (unique) break;
            }
        }
        code.clear();
    }
    if (code.empty())
        return "{\"success\":false,\"errcode\":\"ROOM_CREATE_FAILED\",\"message\":\"生成房间码失败，请重试\"}";

    std::string roomLabel = roomName.empty() ? code : roomName;
    uint64_t id = Snowflake::generate();
    char ins[512];
    snprintf(ins, sizeof(ins),
             "INSERT INTO rooms (id,room_code,room_name,creator_email,controller_email,status) "
             "VALUES (%llu,'%s','%s','%s','%s',0)",
             id, esc(db, code).c_str(), esc(db, roomLabel).c_str(),
             esc(db, email).c_str(), esc(db, email).c_str());

    if (mysql_query(db, ins) != 0) {
        Logger::err("SyncService", std::string("createRoom insert: ") + mysql_error(db));
        return "{\"success\":false,\"errcode\":\"DB_ERROR\",\"message\":\"创建房间失败\"}";
    }

    // 初始化房间同步状态
    auto state = getOrCreateState(code);
    state->controllerEmail = email;

    // ── 4. 缓存到 Redis（1 小时 TTL）──
    try {
        RedisClient::instance().setex("user_room:" + email, 3600, code);
    } catch (...) {}

    std::ostringstream resp;
    resp << "{\"success\":true"
         << ",\"room_code\":\"" << code << "\""
         << ",\"room_name\":\"" << roomLabel << "\""
         << ",\"your_role\":\"controller\""
         << ",\"member_count\":1,\"member_max\":2"
         << "}";
    Logger::info("SyncService", std::string("Room ") + code + " created by " + email);
    return resp.str();
}

std::string SyncService::joinRoom(const std::string &roomCode, const std::string &email) {
    if (roomCode.empty() || email.empty())
        return "{\"success\":false,\"errcode\":\"FIELDS_REQUIRED\",\"message\":\"房间码和邮箱不能为空\"}";

    auto guard = m_pool.acquire(5000);
    if (!guard)
        return "{\"success\":false,\"errcode\":\"DB_ERROR\",\"message\":\"数据库繁忙\"}";
    MYSQL *db = guard->get();

    std::string eCode = esc(db, roomCode);

    // 查询房间
    char q[512];
    snprintf(q, sizeof(q),
        "SELECT id,room_name,creator_email,controller_email,controlled_email,status "
        "FROM rooms WHERE room_code='%s'", eCode.c_str());
    if (mysql_query(db, q) != 0)
        return "{\"success\":false,\"errcode\":\"DB_ERROR\",\"message\":\"查询房间失败\"}";

    MYSQL_RES *res = mysql_store_result(db);
    if (!res || mysql_num_rows(res) == 0) {
        if (res) mysql_free_result(res);
        return "{\"success\":false,\"errcode\":\"ROOM_NOT_FOUND\",\"message\":\"房间不存在\"}";
    }

    MYSQL_ROW row = mysql_fetch_row(res);
    std::string roomName   = row[1] ? row[1] : "";
    std::string controller = row[3] ? row[3] : "";
    std::string controlled = row[4] ? row[4] : "";
    int         status     = row[5] ? atoi(row[5]) : 0;
    mysql_free_result(res);

    // 边界检查
    if (status == 2)
        return "{\"success\":false,\"errcode\":\"ROOM_CLOSED\",\"message\":\"房间已关闭\"}";
    if (email == controller)
        return "{\"success\":false,\"errcode\":\"SELF_JOIN\",\"message\":\"不能加入自己创建的房间\"}";
    if (!controlled.empty() && controlled != email)
        return "{\"success\":false,\"errcode\":\"ROOM_FULL\",\"message\":\"房间已满\"}";

    // 检查加入者是否已在其他房间
    {
        std::string eEmail = esc(db, email);
        char cq[512];
        snprintf(cq, sizeof(cq),
            "SELECT COUNT(*) FROM rooms WHERE (controller_email='%s' OR controlled_email='%s') AND status != 2",
            eEmail.c_str(), eEmail.c_str());
        if (mysql_query(db, cq) == 0) {
            MYSQL_RES *cr = mysql_store_result(db);
            if (cr) {
                MYSQL_ROW rr = mysql_fetch_row(cr);
                if (rr && atoi(rr[0]) > 0) {
                    mysql_free_result(cr);
                    // 自动退出旧房间
                    char lq[512];
                    snprintf(lq, sizeof(lq),
                        "SELECT room_code FROM rooms WHERE (controller_email='%s' OR controlled_email='%s') AND status != 2 LIMIT 1",
                        eEmail.c_str(), eEmail.c_str());
                    if (mysql_query(db, lq) == 0) {
                        MYSQL_RES *or2 = mysql_store_result(db);
                        if (or2) {
                            MYSQL_ROW r2 = mysql_fetch_row(or2);
                            if (r2 && r2[0]) {
                                std::string oldRoom = r2[0];
                                char xq[256];
                                snprintf(xq, sizeof(xq), "UPDATE rooms SET status=2 WHERE room_code='%s'", esc(db, oldRoom).c_str());
                                mysql_query(db, xq);
                                removeState(oldRoom, true);
                                // 清理旧房间 Redis 缓存
                                try { RedisClient::instance().del("user_room:" + email); } catch (...) {}
                            }
                            mysql_free_result(or2);
                        }
                    }
                } else {
                    mysql_free_result(cr);
                }
            }
        }
    }

    // 原子加入（WHERE controlled_email IS NULL → 防竞态）
    std::string eEmail = esc(db, email);
    char upd[512];
    snprintf(upd, sizeof(upd),
        "UPDATE rooms SET controlled_email='%s', status=1 "
        "WHERE room_code='%s' AND controlled_email IS NULL AND status=0",
        eEmail.c_str(), eCode.c_str());
    mysql_query(db, upd);
    if (mysql_affected_rows(db) == 0) {
        return "{\"success\":false,\"errcode\":\"ROOM_FULL\",\"message\":\"房间已被占用\"}";
    }

    // 更新内存状态
    auto state = getOrCreateState(roomCode);
    state->controlledEmail = email;

    // 加入者也缓存到 Redis（防止重复创建房间）
    try {
        RedisClient::instance().setex("user_room:" + email, 3600, roomCode);
    } catch (...) {}

    std::ostringstream resp;
    resp << "{\"success\":true"
         << ",\"room_code\":\"" << roomCode << "\""
         << ",\"room_name\":\"" << roomName << "\""
         << ",\"your_role\":\"controlled\""
         << ",\"controller_email\":\"" << controller << "\""
         << ",\"member_count\":2,\"member_max\":2"
         << "}";
    Logger::info("SyncService", std::string("Room ") + roomCode + ": " + email + " joined");
    return resp.str();
}

std::string SyncService::leaveRoom(const std::string &roomCode, const std::string &email) {
    if (roomCode.empty() || email.empty())
        return "{\"success\":false,\"errcode\":\"FIELDS_REQUIRED\",\"message\":\"参数不完整\"}";

    auto guard = m_pool.acquire(5000);
    if (!guard)
        return "{\"success\":false,\"errcode\":\"DB_ERROR\",\"message\":\"数据库繁忙\"}";
    MYSQL *db = guard->get();

    // 查房间
    char q[512];
    snprintf(q, sizeof(q),
        "SELECT controller_email,controlled_email,status FROM rooms WHERE room_code='%s'",
        esc(db, roomCode).c_str());
    if (mysql_query(db, q) != 0)
        return "{\"success\":false,\"errcode\":\"DB_ERROR\",\"message\":\"查询房间失败\"}";

    MYSQL_RES *res = mysql_store_result(db);
    if (!res || mysql_num_rows(res) == 0) {
        if (res) mysql_free_result(res);
        return "{\"success\":false,\"errcode\":\"ROOM_NOT_FOUND\",\"message\":\"房间不存在\"}";
    }

    MYSQL_ROW row = mysql_fetch_row(res);
    std::string controller = row[0] ? row[0] : "";
    std::string controlled = row[1] ? row[1] : "";
    int status = row[2] ? atoi(row[2]) : 0;
    mysql_free_result(res);

    if (status == 2)
        return "{\"success\":false,\"errcode\":\"ROOM_CLOSED\",\"message\":\"房间已关闭\"}";

    // 验证用户是房间成员
    bool isController = (email == controller);
    bool isControlled = (email == controlled);
    if (!isController && !isControlled)
        return "{\"success\":false,\"errcode\":\"NOT_MEMBER\",\"message\":\"你不在该房间中\"}";

    // 控制方退出 → 关闭房间，通知受控方
    if (isController) {
        snprintf(q, sizeof(q), "UPDATE rooms SET status=2 WHERE room_code='%s'",
                 esc(db, roomCode).c_str());
        mysql_query(db, q);
        // notifyClosed=true → 正在 pull 的受控方会收到 room_closed 信号
        removeState(roomCode, true);
        // 清理双方 Redis 缓存
        try {
            RedisClient::instance().del("user_room:" + controller);
            if (!controlled.empty())
                RedisClient::instance().del("user_room:" + controlled);
        } catch (...) {}
        Logger::info("SyncService", std::string("Room ") + roomCode + " closed by controller " + email);
        return "{\"success\":true,\"message\":\"房间已关闭\"}";
    }

    // 受控方退出
    char upd[512];
    snprintf(upd, sizeof(upd),
        "UPDATE rooms SET controlled_email=NULL,status=0 WHERE room_code='%s'",
        esc(db, roomCode).c_str());
    mysql_query(db, upd);

    // 清理受控方 Redis 缓存
    try { RedisClient::instance().del("user_room:" + email); } catch (...) {}

    // 通知控制方（受控方已离开）
    auto state = findState(roomCode);
    if (state) {
        state->controlledEmail.clear();
        state->cv.notify_all();
    }

    return "{\"success\":true,\"message\":\"已退出房间\"}";
}

std::string SyncService::roomStatus(const std::string &roomCode) {
    if (roomCode.empty())
        return "{\"success\":false,\"errcode\":\"FIELDS_REQUIRED\",\"message\":\"缺少房间码\"}";

    auto guard = m_pool.acquire(5000);
    if (!guard)
        return "{\"success\":false,\"errcode\":\"DB_ERROR\",\"message\":\"数据库繁忙\"}";
    MYSQL *db = guard->get();

    char q[512];
    snprintf(q, sizeof(q),
        "SELECT room_code,room_name,creator_email,controller_email,controlled_email,status "
        "FROM rooms WHERE room_code='%s'", esc(db, roomCode).c_str());
    if (mysql_query(db, q) != 0)
        return "{\"success\":false,\"errcode\":\"DB_ERROR\",\"message\":\"查询失败\"}";

    MYSQL_RES *res = mysql_store_result(db);
    if (!res || mysql_num_rows(res) == 0) {
        if (res) mysql_free_result(res);
        return "{\"success\":false,\"errcode\":\"ROOM_NOT_FOUND\",\"message\":\"房间不存在\"}";
    }

    MYSQL_ROW row = mysql_fetch_row(res);
    std::string code       = row[0] ? row[0] : "";
    std::string name       = row[1] ? row[1] : "";
    std::string creator    = row[2] ? row[2] : "";
    std::string controller = row[3] ? row[3] : "";
    std::string controlled = row[4] ? row[4] : "";
    int         status     = row[5] ? atoi(row[5]) : 0;
    mysql_free_result(res);

    int mc = 0;
    if (!controller.empty()) mc++;
    if (!controlled.empty()) mc++;

    std::ostringstream resp;
    resp << "{\"success\":true"
         << ",\"room_code\":\"" << code << "\""
         << ",\"room_name\":\"" << name << "\""
         << ",\"creator_email\":\"" << creator << "\""
         << ",\"controller_email\":\"" << controller << "\""
         << ",\"controlled_email\":\"" << controlled << "\""
         << ",\"member_count\":" << mc
         << ",\"member_max\":2"
         << ",\"status\":" << status
         << "}";
    return resp.str();
}

// ═══════════════════════════════════════════════════════════
//  坐标中转（含授权检查）
// ═══════════════════════════════════════════════════════════

std::string SyncService::pushCommand(const std::string &roomCode, const std::string &email,
                                      const std::string &opType, double x, double y,
                                      const std::string &extra) {
    auto state = findState(roomCode);
    if (!state)
        return "{\"success\":false,\"errcode\":\"ROOM_NOT_FOUND\",\"message\":\"房间不存在\"}";

    // 授权：必须是该房间的控制方
    if (state->controllerEmail.empty() || state->controllerEmail != email)
        return "{\"success\":false,\"errcode\":\"FORBIDDEN\",\"message\":\"只有控制方能推送坐标\"}";

    {
        std::lock_guard<std::mutex> lk(state->mtx);
        state->cmd.opType = opType;
        state->cmd.extra  = extra;
        state->cmd.x      = x;
        state->cmd.y      = y;
        state->cmd.ts     = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
        state->seq++;
        state->dirty = true;
    }

    state->cv.notify_all();
    return "{\"success\":true,\"message\":\"坐标已同步\"}";
}

std::string SyncService::pullCommand(const std::string &roomCode, const std::string &email,
                                      uint64_t lastSeq, int waitMs) {
    auto state = findState(roomCode);
    if (!state)
        return "{\"success\":false,\"errcode\":\"ROOM_NOT_FOUND\",\"message\":\"房间不存在\"}";

    // 授权：必须是该房间的受控方（且已有人加入）
    if (state->controlledEmail.empty() || state->controlledEmail != email)
        return "{\"success\":false,\"errcode\":\"FORBIDDEN\",\"message\":\"只有受控方能拉取坐标\"}";

    std::string result;

    {
        std::unique_lock<std::mutex> lk(state->mtx);

        // 检查房间是否已被关闭
        if (state->roomClosed)
            return "{\"room_closed\":true}";

        if (state->dirty && state->seq > lastSeq) {
            state->dirty = false;
            result = serializeCommand(state->cmd, state->seq);
            return result;
        }

        if (waitMs <= 0)
            return "";

        bool gotNew = state->cv.wait_for(lk, std::chrono::milliseconds(waitMs), [&]() {
            return state->dirty && state->seq > lastSeq;
        });

        if (gotNew) {
            state->dirty = false;
            result = serializeCommand(state->cmd, state->seq);
        } else if (state->roomClosed) {
            return "{\"room_closed\":true}";
        }
    }

    return result;
}
