#pragma once
#include <string>
#include <cstdint>
#include <vector>

class DbPool;

// ── 兑换码数据访问层 (2026-09-09 收敛自 RedeemRoutes 手写事务) ──
// route 层只做: 参数解析 + Redis 限流 + 组 JSON; 码规范化与事务全在本类。
// 一致性设计 (与旧实现一致, 勿破坏):
//   - MySQL 事务保证"扣次数+加时长"原子
//   - redeem_records 唯一键 uk_user_code 是防重复兑换的总闸 (连点双发也挡住)
//   - 条件 UPDATE (used_count<max_uses) + affected_rows==1 原子抢占最后一份
class RedeemStore {
public:
    struct RedeemResult {
        bool        ok = false;
        std::string errcode;
        std::string msg;
        uint64_t    days = 0;
        std::string newExpires;  // 回读的新订阅到期时间 (空=回读失败, 不影响兑换成功)
    };

    explicit RedeemStore(DbPool &pool) : m_pool(pool) {}

    // 兑换入口: 码规范化(全角→半角/trim/大写) → 格式校验 → 事务兑换 → 回读到期时间
    RedeemResult redeem(uint64_t uid, const std::string &rawCode);

    // ── 管理操作 (2026-09-09 收敛自 SystemRoutes admin 直连 SQL) ──
    struct GenResult {
        bool ok = false; std::string errcode, msg;
        std::vector<std::string> codes;   // 已生成 (碰撞重试 3 次/码)
    };
    GenResult generate(const std::string &prefix, uint64_t days, uint64_t count,
                       uint64_t maxUses, uint64_t expDays);

    struct CodeRow {
        std::string code;
        uint64_t days = 0, maxUses = 0, usedCount = 0;
        int isActive = 0;
        std::string expiresAt, createdAt;
    };
    struct ListResult {
        bool ok = false; std::string errcode, msg;
        std::vector<CodeRow> rows;
        uint64_t total = 0, active = 0, totalUsed = 0;
    };
    ListResult list(uint64_t page);

    // 停用 (不影响已兑换用户); notFound=true 表示码不存在
    struct DisableResult {
        bool ok = false, notFound = false;
        std::string errcode, msg;
        std::string code;   // 规范化后 (trim+大写) 的码, 供成功消息回显
    };
    DisableResult disable(const std::string &code);

private:
    DbPool &m_pool;
};
