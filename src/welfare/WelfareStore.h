#pragma once
#include <string>
#include <vector>
#include <cstdint>

class DbPool;

// ── 福利数据访问层 (2026-09-06 收敛自 WelfareRoutes 直连 SQL) ──
// route 层只调本类, 不再直接写 welfare_events/welfare_claims/users SQL。
class WelfareStore {
public:
    struct WelfareEvent {
        uint64_t    id = 0;
        std::string name;
        std::string desc;
        std::string type;        // event_type
        std::string starts;      // starts_at
        std::string expires;     // expires_at
        uint64_t    amount = 0;  // reward_amount (订阅天数)
        int         claimed = 0; // 当前用户是否已领取
    };

    struct ClaimResult {
        bool        ok = false;
        std::string errcode;
        std::string msg;
        uint64_t    days = 0;
    };

    explicit WelfareStore(DbPool &pool) : m_pool(pool) {}

    // 可领取列表 (target_email 定向福利按用户 email 原文匹配, 内部转义)
    std::vector<WelfareEvent> listAvailable(uint64_t uid, const std::string &email) const;

    // 领取: 校验活动有效/未领过 → 插入 welfare_claims → 发放订阅天数 → 返回新到期时间
    ClaimResult claim(uint64_t uid, uint64_t eventId, const std::string &email);

    // ── 管理定向发放 (2026-09-09 收敛自 SystemRoutes admin 直连 SQL) ──
    // 定向专属福利: 发到指定用户, 仅该用户可见可领; 领取=加 N 天 Pro
    bool grantTargeted(const std::string &email, int days, const std::string &name,
                       const std::string &desc, int expiresDays);

    // 按累计充值金额批量发福利; 幂等: 同用户+同福利名且未过期 → 跳过
    struct GrantByAmountResult { int granted = 0; int skipped = 0; };
    GrantByAmountResult grantByAmount(long long minYuan, int days, const std::string &name,
                                      const std::string &desc, int expiresDays);

private:
    DbPool &m_pool;
};
