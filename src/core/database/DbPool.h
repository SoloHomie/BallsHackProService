#pragma once
// ── MySQL 固定大小连接池（header-only，RAII 守卫）─────────
// 线程安全。每个线程 acquire() 拿到独占连接，用完后自动归还。
//
// 用法:
//   DbPool pool(host, user, pass, db, port, 10);
//   auto guard = pool.acquire(5000);
//   if (guard) {
//       MYSQL* db = guard->get();
//       // ... 执行查询 ...
//   }  // guard 析构，连接归还池

#include <mysql/mysql.h>
#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <memory>
#include <optional>
#include <iostream>

class DbPool {
public:
    // ── RAII 连接守卫 ──────────────────────────────────
    class ConnGuard {
    public:
        ConnGuard(MYSQL* conn, DbPool* pool)
            : m_conn(conn), m_pool(pool) {}

        ~ConnGuard() {
            if (m_conn && m_pool)
                m_pool->release(m_conn);
        }

        ConnGuard(const ConnGuard&)            = delete;
        ConnGuard& operator=(const ConnGuard&) = delete;
        ConnGuard(ConnGuard&& other) noexcept
            : m_conn(other.m_conn), m_pool(other.m_pool) {
            other.m_conn = nullptr;
            other.m_pool = nullptr;
        }
        ConnGuard& operator=(ConnGuard&& other) noexcept {
            if (this != &other) {
                if (m_conn && m_pool) m_pool->release(m_conn);
                m_conn = other.m_conn;
                m_pool = other.m_pool;
                other.m_conn = nullptr;
                other.m_pool = nullptr;
            }
            return *this;
        }

        MYSQL* get() const { return m_conn; }
        MYSQL* operator->() const { return m_conn; }

    private:
        MYSQL* m_conn;
        DbPool* m_pool;
    };

    // ── 构造函数 ────────────────────────────────────────
    DbPool(const std::string& host,
           const std::string& user,
           const std::string& password,
           const std::string& database,
           uint16_t port,
           int poolSize)
        : m_poolSize(poolSize), m_running(true)
        , m_host(host), m_user(user), m_password(password)
        , m_database(database), m_port(port) {
        for (int i = 0; i < poolSize; ++i) {
            MYSQL* conn = createConn(host, user, password, database, port);
            if (conn) {
                m_available.push(conn);
            }
        }
        if (m_available.empty()) {
            std::cerr << "[DbPool] WARNING: 0/" << poolSize
                      << " connections created" << std::endl;
        }
    }

    ~DbPool() {
        m_running = false;
        m_cv.notify_all();
        while (!m_available.empty()) {
            MYSQL* c = m_available.front();
            m_available.pop();
            mysql_close(c);
        }
    }

    DbPool(const DbPool&)            = delete;
    DbPool& operator=(const DbPool&) = delete;

    // ── 获取连接（超时毫秒，返回 nullopt 表示超时）───────
    std::optional<ConnGuard> acquire(int timeoutMs) {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                           [this] { return !m_available.empty() || !m_running; }))
            return std::nullopt;
        if (m_available.empty()) return std::nullopt;
        MYSQL* conn = m_available.front();
        m_available.pop();
        // Health check: ping before returning. If dead, recreate connection.
        if (mysql_ping(conn) != 0) {
            mysql_close(conn);
            // Try to recreate a new connection
            MYSQL* newConn = createConn(m_host, m_user, m_password, m_database, m_port);
            if (newConn) {
                std::cerr << "[DbPool] recreated dead connection" << std::endl;
                return ConnGuard(newConn, this);
            }
            std::cerr << "[DbPool] failed to recreate connection" << std::endl;
            return std::nullopt;
        }
        return ConnGuard(conn, this);
    }

    int size() const { return m_poolSize; }

private:
    void release(MYSQL* conn) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_available.push(conn);
        m_cv.notify_one();
    }

    static MYSQL* createConn(const std::string& host,
                             const std::string& user,
                             const std::string& password,
                             const std::string& database,
                             uint16_t port) {
        MYSQL* conn = mysql_init(nullptr);
        if (!conn) return nullptr;

        // 3 秒超时: 共享云盘卡顿时不无限等待, 快速失败交给上层/客户端重试
        unsigned int timeoutSec = 3;
        mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeoutSec);
        mysql_options(conn, MYSQL_OPT_READ_TIMEOUT, &timeoutSec);
        mysql_options(conn, MYSQL_OPT_WRITE_TIMEOUT, &timeoutSec);

        // 连接（重试 3 次，间隔 2 秒，等 MySQL 就绪）
        for (int retry = 0; retry < 3; ++retry) {
            if (mysql_real_connect(conn, host.c_str(), user.c_str(),
                                   password.c_str(), database.c_str(),
                                   port, nullptr, 0)) {
                mysql_set_character_set(conn, "utf8mb4");
                return conn;
            }
            if (retry < 2) {
                std::cerr << "[DbPool] 连接失败 (尝试 " << (retry + 1)
                          << "/3): " << mysql_error(conn)
                          << " — 2秒后重试..." << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        }

        std::cerr << "[DbPool] 连接失败: " << mysql_error(conn) << std::endl;
        mysql_close(conn);
        return nullptr;
    }

    int m_poolSize;
    std::atomic<bool> m_running;
    std::queue<MYSQL*> m_available;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::string m_host, m_user, m_password, m_database;
    uint16_t m_port;
};
