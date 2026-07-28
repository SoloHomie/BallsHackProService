#pragma once
// ── Snowflake ID 生成器（header-only，线程安全）────────────
// 结构: [41bit 毫秒时间戳] [5bit 数据中心] [5bit 工作节点] [12bit 序列号]
// 单机每秒可生成 409.6 万个唯一 ID，无需数据库自增。
//
// 用法:
//   uint64_t id = Snowflake::generate();  // 线程安全

#include <cstdint>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace Snowflake {

    // 可自定义（多机部署时每台机器设不同值）
    constexpr uint64_t DATACENTER_ID = 1;
    constexpr uint64_t WORKER_ID     = 1;

    // Snowflake 纪元 (2024-01-01 00:00:00 UTC, 毫秒)
    constexpr uint64_t EPOCH = 1704067200000ULL;

    // 位宽
    constexpr int TIMESTAMP_BITS  = 41;
    constexpr int DATACENTER_BITS = 5;
    constexpr int WORKER_BITS     = 5;
    constexpr int SEQUENCE_BITS   = 12;
    constexpr uint64_t MAX_SEQUENCE = (1ULL << SEQUENCE_BITS) - 1;  // 4095

    constexpr int WORKER_SHIFT     = SEQUENCE_BITS;                   // 12
    constexpr int DATACENTER_SHIFT = SEQUENCE_BITS + WORKER_BITS;     // 17
    constexpr int TIMESTAMP_SHIFT  = SEQUENCE_BITS + WORKER_BITS
                                     + DATACENTER_BITS;               // 22

    inline uint64_t nowMs() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count());
    }

    // ── 线程安全生成器 ──────────────────────────────────
    inline uint64_t generate() {
        static std::atomic<uint64_t> s_lastTimestamp{0};
        static std::atomic<uint64_t> s_sequence{0};
        static std::mutex s_mtx;

        std::lock_guard<std::mutex> lock(s_mtx);
        uint64_t ts = nowMs();

        if (ts == s_lastTimestamp.load()) {
            // 同一毫秒内，递增序列号
            uint64_t seq = (s_sequence.load() + 1) & MAX_SEQUENCE;
            s_sequence.store(seq);
            if (seq == 0) {
                // 序列号用完，等到下一毫秒
                while (nowMs() <= s_lastTimestamp.load()) {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
                ts = nowMs();
            }
        } else {
            s_sequence.store(0);
        }

        s_lastTimestamp.store(ts);

        return ((ts - EPOCH) << TIMESTAMP_SHIFT)
             | (DATACENTER_ID << DATACENTER_SHIFT)
             | (WORKER_ID   << WORKER_SHIFT)
             | s_sequence.load();
    }

    // ── 从 ID 中提取时间戳（调试用）─────────────────────
    inline uint64_t extractTimestamp(uint64_t id) {
        return (id >> TIMESTAMP_SHIFT) + EPOCH;
    }
}
