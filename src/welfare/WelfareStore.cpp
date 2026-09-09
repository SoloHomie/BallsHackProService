#include "WelfareStore.h"
#include "../common/Snowflake.h"
#include "../core/database/DbPool.h"
#include <mysql/mysql.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iostream>

// ── 取用户 email 并转义 (查不到则空串 = 仅匹配全员福利) ──
static void loadEmailEscaped(MYSQL *db, uint64_t uid, char *eesc, size_t eescSz) {
    eesc[0] = '\0';
    char qe[256];
    snprintf(qe, sizeof(qe), "SELECT email FROM users WHERE id=%llu",
             (unsigned long long)uid);
    mysql_query(db, qe);
    MYSQL_RES *re = mysql_store_result(db);
    if (re) {
        MYSQL_ROW rw = mysql_fetch_row(re);
        if (rw && rw[0])
            mysql_real_escape_string(db, eesc, rw[0],
                                     std::min<size_t>(strlen(rw[0]), eescSz - 1));
        mysql_free_result(re);
    }
}

// ═══════════════════════════════════════════════════════
//  listAvailable — 可领取列表
//  2026-08-16 移除补种: 接口不再 INSERT IGNORE 新人礼包,
//  福利数据以运营库为准 (关闭福利请 UPDATE is_active=0 而非 DELETE)。
// ═══════════════════════════════════════════════════════
std::vector<WelfareStore::WelfareEvent>
WelfareStore::listAvailable(uint64_t uid, const std::string &email) const {
    std::vector<WelfareEvent> out;
    auto guard = m_pool.acquire(5000);
    if (!guard) return out;
    MYSQL *db = guard->get();

    char eesc[256];
    loadEmailEscaped(db, uid, eesc, sizeof(eesc));
    (void)email;  // email 原文由 SQL 现查 (保持与旧实现一致的单一来源)

    char q[1024];
    snprintf(q, sizeof(q),
        "SELECT e.id, e.name, e.description, e.event_type, e.reward_amount, "
        "e.starts_at, e.expires_at, "
        "CASE WHEN c.id IS NOT NULL THEN 1 ELSE 0 END AS claimed "
        "FROM welfare_events e "
        "LEFT JOIN welfare_claims c ON e.id = c.event_id AND c.user_id = %llu "
        "WHERE e.is_active = 1 AND e.starts_at <= NOW() AND e.expires_at >= NOW() "
        "AND (e.target_email IS NULL OR e.target_email='%s') "
        "ORDER BY e.event_type = 'new_user' DESC, e.starts_at ASC",
        (unsigned long long)uid, eesc);
    mysql_query(db, q);
    MYSQL_RES *res = mysql_store_result(db);
    if (!res) return out;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        WelfareEvent e;
        e.id      = std::stoull(row[0] ? row[0] : "0");
        e.name    = row[1] ? row[1] : "";
        e.desc    = row[2] ? row[2] : "";
        e.type    = row[3] ? row[3] : "";
        e.amount  = std::stoull(row[4] ? row[4] : "0");
        e.starts  = row[5] ? row[5] : "";
        e.expires = row[6] ? row[6] : "";
        e.claimed = row[7] ? std::stoi(row[7]) : 0;
        out.push_back(std::move(e));
    }
    mysql_free_result(res);
    return out;
}

// ═══════════════════════════════════════════════════════
//  claim — 领取福利 (2026-08-26 定向专属福利: 仅目标用户可领)
// ═══════════════════════════════════════════════════════
WelfareStore::ClaimResult
WelfareStore::claim(uint64_t uid, uint64_t eventId, const std::string &email) {
    ClaimResult r;
    auto guard = m_pool.acquire(5000);
    if (!guard) { r.errcode = "DB_ERROR"; r.msg = "数据库繁忙"; return r; }
    MYSQL *db = guard->get();

    char eesc[256];
    loadEmailEscaped(db, uid, eesc, sizeof(eesc));
    (void)email;  // 同上, email 由 SQL 现查

    char q[512];
    // 1. 活动有效性
    snprintf(q, sizeof(q),
        "SELECT id, reward_amount FROM welfare_events "
        "WHERE id=%llu AND is_active=1 AND starts_at<=NOW() AND expires_at>=NOW() "
        "AND (target_email IS NULL OR target_email='%s')",
        (unsigned long long)eventId, eesc);
    mysql_query(db, q);
    MYSQL_RES *res = mysql_store_result(db);
    if (!res || mysql_num_rows(res) == 0) {
        if (res) mysql_free_result(res);
        r.errcode = "NOT_AVAILABLE"; r.msg = "该福利已失效";
        return r;
    }
    MYSQL_ROW row = mysql_fetch_row(res);
    uint64_t days = std::stoull(row[1] ? row[1] : "0");
    mysql_free_result(res);

    // 2. 已领取检查
    snprintf(q, sizeof(q),
        "SELECT id FROM welfare_claims WHERE user_id=%llu AND event_id=%llu",
        (unsigned long long)uid, (unsigned long long)eventId);
    mysql_query(db, q);
    res = mysql_store_result(db);
    if (res && mysql_num_rows(res) > 0) {
        mysql_free_result(res);
        r.errcode = "ALREADY_CLAIMED"; r.msg = "你已经领取过该福利啦~";
        return r;
    }
    if (res) mysql_free_result(res);

    // 3. 落领取记录
    uint64_t cid = Snowflake::generate();
    snprintf(q, sizeof(q),
        "INSERT INTO welfare_claims (id, user_id, event_id) VALUES (%llu,%llu,%llu)",
        (unsigned long long)cid, (unsigned long long)uid, (unsigned long long)eventId);
    if (mysql_query(db, q) != 0) {
        r.errcode = "CLAIM_FAILED"; r.msg = "领取失败，请稍后重试";
        return r;
    }

    // 4. 发放 Pro 订阅天数
    snprintf(q, sizeof(q),
        "UPDATE users SET subscription_expires = "
        "DATE_ADD(GREATEST(COALESCE(subscription_expires, NOW()), NOW()), INTERVAL %llu DAY) "
        "WHERE id=%llu", (unsigned long long)days, (unsigned long long)uid);
    mysql_query(db, q);

    r.ok = true;
    r.days = days;
    std::cout << "[Welfare] claim uid=" << uid << " days=" << days << std::endl;
    return r;
}

// ═══════════════════════════════════════════════════════
//  管理定向发放 (2026-09-09 收敛自 SystemRoutes admin 直连 SQL,
//  SQL 逐字保留)
// ═══════════════════════════════════════════════════════
bool WelfareStore::grantTargeted(const std::string &email, int days,
                                 const std::string &name, const std::string &desc,
                                 int expiresDays) {
    auto guard = m_pool.acquire(5000);
    if (!guard) return false;
    MYSQL *db = guard->get();

    char q[1024]; char eesc[256]; char neesc[128]; char descsc[512];
    mysql_real_escape_string(db, eesc, email.c_str(), email.size());
    mysql_real_escape_string(db, neesc, name.c_str(), name.size());
    mysql_real_escape_string(db, descsc, desc.c_str(), desc.size());
    uint64_t eid = Snowflake::generate();
    snprintf(q, sizeof(q),
        "INSERT INTO welfare_events (id, name, description, event_type, target_email,"
        " reward_amount, starts_at, expires_at, is_active)"
        " VALUES (%llu, '%s', '%s', 'general', '%s', %d,"
        " NOW(), DATE_ADD(NOW(), INTERVAL %d DAY), 1)",
        eid, neesc, descsc, eesc, days, expiresDays);
    return mysql_query(db, q) == 0;
}

WelfareStore::GrantByAmountResult
WelfareStore::grantByAmount(long long minYuan, int days, const std::string &name,
                            const std::string &desc, int expiresDays) {
    GrantByAmountResult out;
    auto guard = m_pool.acquire(5000);
    if (!guard) return out;
    MYSQL *db = guard->get();

    // 1) 累计充值 ≥ 门槛的用户 (amount 单位=分)
    char q[2048];
    snprintf(q, sizeof(q),
        "SELECT u.id, u.email, SUM(p.amount) FROM payment_orders p "
        "JOIN users u ON u.id=p.user_id WHERE p.status='paid' "
        "GROUP BY u.id HAVING SUM(p.amount) >= %lld", minYuan * 100);
    if (mysql_query(db, q) != 0) return out;
    MYSQL_RES *res = mysql_store_result(db);
    if (!res) return out;
    MYSQL_ROW row;
    char neesc[128]; char descsc[512]; char eesc[256];
    while ((row = mysql_fetch_row(res)) != nullptr) {
        uint64_t uid = strtoull(row[0], nullptr, 10);
        std::string email = row[1] ? row[1] : "";
        if (email.empty()) { out.skipped++; continue; }
        mysql_real_escape_string(db, eesc, email.c_str(), email.size());
        mysql_real_escape_string(db, neesc, name.c_str(), name.size());
        mysql_real_escape_string(db, descsc, desc.c_str(), desc.size());
        // 2) 幂等: 同用户同福利名未过期 → 跳过
        snprintf(q, sizeof(q),
            "SELECT COUNT(*) FROM welfare_events WHERE target_email='%s' "
            "AND name='%s' AND is_active=1 AND expires_at>NOW()", eesc, neesc);
        if (mysql_query(db, q) != 0) { out.skipped++; continue; }
        MYSQL_RES *cres = mysql_store_result(db);
        bool dup = false;
        if (cres && mysql_num_rows(cres) > 0) {
            MYSQL_ROW crow = mysql_fetch_row(cres);
            dup = crow && crow[0] && atoll(crow[0]) > 0;
        }
        if (cres) mysql_free_result(cres);
        if (dup) { out.skipped++; continue; }
        // 3) 插入定向福利
        uint64_t eid = Snowflake::generate();
        snprintf(q, sizeof(q),
            "INSERT INTO welfare_events (id, name, description, event_type, target_email,"
            " reward_amount, starts_at, expires_at, is_active)"
            " VALUES (%llu, '%s', '%s', 'general', '%s', %d,"
            " NOW(), DATE_ADD(NOW(), INTERVAL %d DAY), 1)",
            eid, neesc, descsc, eesc, days, expiresDays);
        if (mysql_query(db, q) != 0) { out.skipped++; continue; }
        out.granted++;
    }
    mysql_free_result(res);
    return out;
}
