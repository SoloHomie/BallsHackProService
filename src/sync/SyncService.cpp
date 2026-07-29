#include "SyncService.h"
#include "../common/Snowflake.h"
#include "../common/Logger.h"
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
    mysql_real_escape_string(db, buf, s.c_str(), s.size());
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
    // 先读锁查找
    {
        std::shared_lock<std::shared_mutex> lk(m_roomsMutex);
        auto it = m_rooms.find(roomCode);
        if (it != m_rooms.end()) return it->second;
    }
    // 写锁创建
    {
        std::unique_lock<std::shared_mutex> lk(m_roomsMutex);
        auto it = m_rooms.find(roomCode);
        if (it != m_rooms.end()) return it->second;
        auto state = std::make_shared<RoomSyncState>();
        m_rooms[roomCode] = state;
        return state;
    }
}

void SyncService::removeState(const std::string &roomCode) {
    std::unique_lock<std::shared_mutex> lk(m_roomsMutex);
    auto it = m_rooms.find(roomCode);
    if (it != m_rooms.end()) {
        // 唤醒所有等待者（它们会检测到 dirty=false 并返回空）
        {
            std::lock_guard<std::mutex> rlk(it->second->mtx);
            it->second->dirty = false;
        }
        it->second->cv.notify_all();
        m_rooms.erase(it);
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
    if (roomName.empty() || email.empty())
        return "{\"success\":false,\"errcode\":\"FIELDS_REQUIRED\",\"message\":\"房间名和邮箱不能为空\"}";

    auto guard = m_pool.acquire(5000);
    if (!guard)
        return "{\"success\":false,\"errcode\":\"DB_ERROR\",\"message\":\"数据库繁忙\"}";
    MYSQL *db = guard->get();

    // 生成唯一 room code
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
                if (row && atoi(row[0]) == 0) break;
                mysql_free_result(res);
            }
        }
        code.clear();
    }
    if (code.empty())
        return "{\"success\":false,\"errcode\":\"ROOM_CREATE_FAILED\",\"message\":\"生成房间码失败，请重试\"}";

    uint64_t id = Snowflake::generate();
    char ins[512];
    snprintf(ins, sizeof(ins),
             "INSERT INTO rooms (id,room_code,room_name,creator_email,status) "
             "VALUES (%llu,'%s','%s','%s',0)",
             id, esc(db, code).c_str(), esc(db, roomName).c_str(), esc(db, email).c_str());

    if (mysql_query(db, ins) != 0) {
        Logger::err("SyncService",
                    std::string("createRoom insert failed: ") + mysql_error(db));
        return "{\"success\":false,\"errcode\":\"DB_ERROR\",\"message\":\"创建房间失败\"}";
    }

    std::ostringstream resp;
    resp << "{\"success\":true"
         << ",\"room_code\":\"" << code << "\""
         << ",\"room_name\":\"" << roomName << "\""
         << ",\"creator_email\":\"" << email << "\""
         << ",\"member_count\":1"
         << ",\"member_max\":2"
         << ",\"controller_email\":\"\""
         << ",\"controlled_email\":\"\""
         << "}";
    Logger::info("SyncService",
                 std::string("Room created: ") + code + " by " + email);
    return resp.str();
}

std::string SyncService::joinRoom(const std::string &roomCode, const std::string &email,
                                   const std::string &role) {
    if (roomCode.empty() || email.empty() || role.empty())
        return "{\"success\":false,\"errcode\":\"FIELDS_REQUIRED\",\"message\":\"参数不完整\"}";
    if (role != "controller" && role != "controlled")
        return "{\"success\":false,\"errcode\":\"INVALID_ROLE\",\"message\":\"角色只能是 controller 或 controlled\"}";

    auto guard = m_pool.acquire(5000);
    if (!guard)
        return "{\"success\":false,\"errcode\":\"DB_ERROR\",\"message\":\"数据库繁忙\"}";
    MYSQL *db = guard->get();

    char q[512];
    snprintf(q, sizeof(q),
             "SELECT id,room_name,creator_email,controller_email,controlled_email,status "
             "FROM rooms WHERE room_code='%s'", esc(db, roomCode).c_str());
    if (mysql_query(db, q) != 0)
        return "{\"success\":false,\"errcode\":\"DB_ERROR\",\"message\":\"查询房间失败\"}";

    MYSQL_RES *res = mysql_store_result(db);
    if (!res || mysql_num_rows(res) == 0) {
        if (res) mysql_free_result(res);
        return "{\"success\":false,\"errcode\":\"ROOM_NOT_FOUND\",\"message\":\"房间不存在\"}";
    }

    MYSQL_ROW row = mysql_fetch_row(res);
    std::string roomName   = row[1] ? row[1] : "";
    std::string creator    = row[2] ? row[2] : "";
    std::string controller = row[3] ? row[3] : "";
    std::string controlled = row[4] ? row[4] : "";
    mysql_free_result(res);

    const std::string &targetField = (role == "controller") ? controller : controlled;
    if (!targetField.empty() && targetField != email)
        return "{\"success\":false,\"errcode\":\"ROLE_TAKEN\",\"message\":\"该角色已被其他人占用\"}";

    const char *col = (role == "controller") ? "controller_email" : "controlled_email";
    char upd[512];
    snprintf(upd, sizeof(upd),
             "UPDATE rooms SET %s='%s', status=1 WHERE room_code='%s'",
             col, email.c_str(), esc(db, roomCode).c_str());
    mysql_query(db, upd);

    // 重新统计
    snprintf(q, sizeof(q),
             "SELECT controller_email,controlled_email FROM rooms WHERE room_code='%s'",
             esc(db, roomCode).c_str());
    mysql_query(db, q);
    res = mysql_store_result(db);
    int memberCount = 0;
    std::string curController, curControlled;
    if (res) {
        MYSQL_ROW r = mysql_fetch_row(res);
        if (r) {
            curController = r[0] ? r[0] : "";
            curControlled = r[1] ? r[1] : "";
            if (!curController.empty()) memberCount++;
            if (!curControlled.empty()) memberCount++;
        }
        mysql_free_result(res);
    }

    // 初始化房间的同步状态
    getOrCreateState(roomCode);

    std::ostringstream resp;
    resp << "{\"success\":true"
         << ",\"room_code\":\"" << roomCode << "\""
         << ",\"room_name\":\"" << roomName << "\""
         << ",\"creator_email\":\"" << creator << "\""
         << ",\"controller_email\":\"" << curController << "\""
         << ",\"controlled_email\":\"" << curControlled << "\""
         << ",\"member_count\":" << memberCount
         << ",\"member_max\":2"
         << ",\"your_role\":\"" << role << "\""
         << "}";
    Logger::info("SyncService",
                 std::string("Room ") + roomCode + ": " + email + " joined as " + role);
    return resp.str();
}

std::string SyncService::leaveRoom(const std::string &roomCode, const std::string &email) {
    if (roomCode.empty() || email.empty())
        return "{\"success\":false,\"errcode\":\"FIELDS_REQUIRED\",\"message\":\"参数不完整\"}";

    auto guard = m_pool.acquire(5000);
    if (!guard)
        return "{\"success\":false,\"errcode\":\"DB_ERROR\",\"message\":\"数据库繁忙\"}";
    MYSQL *db = guard->get();

    char upd[512];
    snprintf(upd, sizeof(upd),
             "UPDATE rooms SET "
             "controller_email = IF(controller_email='%s', NULL, controller_email), "
             "controlled_email = IF(controlled_email='%s', NULL, controlled_email) "
             "WHERE room_code='%s'",
             email.c_str(), email.c_str(), esc(db, roomCode).c_str());
    mysql_query(db, upd);

    char q[256];
    snprintf(q, sizeof(q),
             "SELECT controller_email,controlled_email FROM rooms WHERE room_code='%s'",
             esc(db, roomCode).c_str());
    mysql_query(db, q);
    MYSQL_RES *res = mysql_store_result(db);
    bool empty = true;
    if (res) {
        MYSQL_ROW row = mysql_fetch_row(res);
        if (row) {
            empty = !(row[0] && strlen(row[0]) > 0) && !(row[1] && strlen(row[1]) > 0);
        }
        mysql_free_result(res);
    }
    if (empty) {
        snprintf(q, sizeof(q), "UPDATE rooms SET status=2 WHERE room_code='%s'", esc(db, roomCode).c_str());
        mysql_query(db, q);
        removeState(roomCode);
    } else {
        // 通知房间内另一个客户端（唤醒长轮询 → 触发人数刷新）
        auto state = getOrCreateState(roomCode);
        state->cv.notify_all();
    }

    Logger::info("SyncService",
                 std::string("Room ") + roomCode + ": " + email + " left" + (empty ? " (closed)" : ""));
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
//  坐标中转（条件变量通知）
// ═══════════════════════════════════════════════════════════

void SyncService::pushCommand(const std::string &roomCode, const std::string &opType,
                               double x, double y, const std::string &extra) {
    auto state = getOrCreateState(roomCode);

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

    // 唤醒所有等待中的 pull 请求
    state->cv.notify_all();
}

std::string SyncService::pullCommand(const std::string &roomCode, uint64_t lastSeq, int waitMs) {
    auto state = getOrCreateState(roomCode);
    std::string result;

    {
        std::unique_lock<std::mutex> lk(state->mtx);

        // 检查是否有新数据
        if (state->dirty && state->seq > lastSeq) {
            state->dirty = false;
            result = serializeCommand(state->cmd, state->seq);
            return result;
        }

        // 不需要等待
        if (waitMs <= 0)
            return "";

        // 阻塞等待通知或超时
        bool gotNew = state->cv.wait_for(lk, std::chrono::milliseconds(waitMs), [&]() {
            return state->dirty && state->seq > lastSeq;
        });

        if (gotNew) {
            state->dirty = false;
            result = serializeCommand(state->cmd, state->seq);
        }
    }

    return result;
}
