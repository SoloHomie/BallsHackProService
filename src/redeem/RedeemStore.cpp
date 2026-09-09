#include "RedeemStore.h"
#include "../core/database/DbPool.h"
#include "../common/Snowflake.h"
#include <mysql/mysql.h>
#include <cctype>
#include <cstdio>
#include <random>
#include <chrono>

// ── 码规范化: 全角→半角 + trim + 大写 (2026-08-16 原实现, 逐字迁移) ──
static bool normalizeCode(const std::string &raw, std::string &code) {
    std::string norm;
    for (size_t i = 0; i < raw.size(); ) {
        unsigned char b = raw[i];
        if (b == 0xE3 && i + 2 < raw.size() && (unsigned char)raw[i+1] == 0x80 && (unsigned char)raw[i+2] == 0x80) {
            norm += ' '; i += 3; continue;                 // U+3000 全角空格
        }
        if (b == 0xEF && i + 2 < raw.size() && (unsigned char)raw[i+1] == 0xBC) {
            unsigned char c3 = raw[i+2];
            if (c3 >= 0x90 && c3 <= 0x99) { norm += (char)('0' + (c3 - 0x90)); i += 3; continue; } // 全角数字
            if (c3 >= 0xA1 && c3 <= 0xBA) { norm += (char)('A' + (c3 - 0xA1)); i += 3; continue; } // 全角大写
            if (c3 == 0x8D) { norm += '-'; i += 3; continue; }                                     // 全角连字符
        }
        norm += (char)b; i += 1;
    }
    size_t s = 0, e = norm.size();
    while (s < e && std::isspace((unsigned char)norm[s])) s++;
    while (e > s && std::isspace((unsigned char)norm[e-1])) e--;
    for (size_t k = s; k < e; k++) code += (char)std::toupper((unsigned char)norm[k]);
    return !code.empty() && code.size() <= 24;
}

// ═══════════════════════════════════════════════════════
//  redeem — 兑换 (事务整体迁移自 RedeemRoutes, SQL 逐字保留)
// ═══════════════════════════════════════════════════════
RedeemStore::RedeemResult RedeemStore::redeem(uint64_t uid, const std::string &rawCode) {
    RedeemResult out;

    std::string code;
    if (!normalizeCode(rawCode, code)) {
        out.errcode = "INVALID_CODE"; out.msg = "请输入有效的兑换码"; return out;
    }
    for (char c : code)
        if (!(std::isalnum((unsigned char)c) || c == '-')) {
            out.errcode = "INVALID_CODE"; out.msg = "兑换码格式不正确"; return out;
        }

    auto guard = m_pool.acquire(5000);
    if (!guard) { out.errcode = "DB_ERROR"; out.msg = "数据库繁忙"; return out; }
    MYSQL *db = guard->get();
    char q[1024]; char eesc[64];
    mysql_real_escape_string(db, eesc, code.c_str(), code.size());

    auto rollbackAnd = [&](const std::string &errcode, const std::string &msg) -> RedeemResult {
        mysql_query(db, "ROLLBACK");
        RedeemResult r; r.errcode = errcode; r.msg = msg; return r;
    };

    if (mysql_query(db, "START TRANSACTION") != 0) {
        out.errcode = "DB_ERROR"; out.msg = "数据库繁忙，请稍后再试"; return out;
    }

    // 1. 查码 (FOR UPDATE 串行化同一码的并发兑换)
    snprintf(q, sizeof(q),
        "SELECT id, days, is_active, max_uses, used_count, expires_at, NOW() "
        "FROM redeem_codes WHERE code='%s' FOR UPDATE", eesc);
    mysql_query(db, q);
    MYSQL_RES *res = mysql_store_result(db);
    if (!res || mysql_num_rows(res) == 0) {
        if (res) mysql_free_result(res);
        return rollbackAnd("INVALID_CODE", "兑换码不存在或已失效");
    }
    MYSQL_ROW row = mysql_fetch_row(res);
    uint64_t codeId  = std::stoull(row[0] ? row[0] : "0");
    uint64_t days    = std::stoull(row[1] ? row[1] : "0");
    bool active      = row[2] && std::stoi(row[2]) != 0;
    uint64_t maxUses = std::stoull(row[3] ? row[3] : "0");
    uint64_t used    = std::stoull(row[4] ? row[4] : "0");
    std::string expAt  = row[5] ? row[5] : "";
    std::string nowStr = row[6] ? row[6] : "";
    mysql_free_result(res);

    // 2a. 同一用户重复兑换检查 (先于耗尽预检, 给出准确提示)
    snprintf(q, sizeof(q),
        "SELECT id FROM redeem_records WHERE user_id=%llu AND code_id=%llu", uid, codeId);
    mysql_query(db, q);
    MYSQL_RES *resDup = mysql_store_result(db);
    bool alreadyRedeemed = resDup && mysql_num_rows(resDup) > 0;
    if (resDup) mysql_free_result(resDup);
    if (alreadyRedeemed)
        return rollbackAnd("ALREADY_REDEEMED", "你已兑换过该兑换码~");

    // 2b. 状态预检
    if (!active)  return rollbackAnd("INVALID_CODE", "兑换码已被停用");
    if (days == 0) return rollbackAnd("INVALID_CODE", "兑换码无有效时长");
    if (!expAt.empty() && !nowStr.empty() && expAt < nowStr)
        return rollbackAnd("CODE_EXPIRED", "兑换码已过期");
    if (maxUses != 0 && used >= maxUses)
        return rollbackAnd("CODE_EXHAUSTED", "兑换码已用完");

    // 3. 落兑换流水 —— 唯一键 uk_user_code 是防重复兑换总闸
    uint64_t rid = Snowflake::generate();
    snprintf(q, sizeof(q),
        "INSERT INTO redeem_records (id, user_id, code_id, code, days) "
        "VALUES (%llu,%llu,%llu,'%s',%llu)", rid, uid, codeId, eesc, days);
    if (mysql_query(db, q) != 0) {
        if (mysql_errno(db) == 1062)
            return rollbackAnd("ALREADY_REDEEMED", "你已兑换过该兑换码~");
        return rollbackAnd("REDEEM_FAILED", "兑换失败，请稍后再试");
    }

    // 4. 条件消费次数 —— affected_rows==1 才原子抢到最后一份
    snprintf(q, sizeof(q),
        "UPDATE redeem_codes SET used_count=used_count+1 "
        "WHERE id=%llu AND is_active=1 "
        "AND (max_uses=0 OR used_count<max_uses) "
        "AND (expires_at IS NULL OR expires_at>=NOW())", codeId);
    if (mysql_query(db, q) != 0 || mysql_affected_rows(db) != 1)
        return rollbackAnd("CODE_EXHAUSTED", "兑换码已用完");

    // 5. 累加订阅时长 (过期从今起算, 未过期累加)
    snprintf(q, sizeof(q),
        "UPDATE users SET subscription_expires = "
        "DATE_ADD(GREATEST(COALESCE(subscription_expires, NOW()), NOW()), INTERVAL %llu DAY) "
        "WHERE id=%llu", days, uid);
    if (mysql_query(db, q) != 0)
        return rollbackAnd("REDEEM_FAILED", "兑换失败，请稍后再试");

    if (mysql_query(db, "COMMIT") != 0) {
        mysql_query(db, "ROLLBACK");
        out.errcode = "REDEEM_FAILED"; out.msg = "兑换失败，请稍后再试"; return out;
    }

    // 6. 回读新到期时间 (供 route 层写订阅缓存)
    snprintf(q, sizeof(q), "SELECT subscription_expires FROM users WHERE id=%llu", uid);
    mysql_query(db, q);
    MYSQL_RES *res2 = mysql_store_result(db);
    if (res2) {
        MYSQL_ROW r2 = mysql_fetch_row(res2);
        if (r2 && r2[0]) out.newExpires = r2[0];
        mysql_free_result(res2);
    }

    out.ok = true;
    out.days = days;
    return out;
}

// ═══════════════════════════════════════════════════════
//  管理操作 (2026-09-09 收敛自 SystemRoutes admin 直连 SQL,
//  SQL 逐字保留)
// ═══════════════════════════════════════════════════════
RedeemStore::GenResult RedeemStore::generate(const std::string &prefix,
                                             uint64_t days, uint64_t count,
                                             uint64_t maxUses, uint64_t expDays) {
    GenResult out;

    // ── 业务校验 (原 handler 规则逐字保留) ──
    if (days < 1 || days > 3650) {
        out.errcode = "FIELDS_REQUIRED"; out.msg = "days 需在 1-3650 天"; return out;
    }
    if (count < 1 || count > 500) {
        out.errcode = "FIELDS_REQUIRED"; out.msg = "count 需在 1-500"; return out;
    }
    if (maxUses > 10000) {
        out.errcode = "FIELDS_REQUIRED"; out.msg = "max_uses 需为 0(不限) 或 1-10000"; return out;
    }
    if (expDays > 3650) {
        out.errcode = "FIELDS_REQUIRED"; out.msg = "expires_days 需在 0-3650"; return out;
    }
    std::string pfx = prefix;
    for (char &c : pfx) c = (char)std::toupper((unsigned char)c);
    if (pfx.size() > 6) {
        out.errcode = "FIELDS_REQUIRED"; out.msg = "prefix 需为 1-6 位字母数字"; return out;
    }
    for (char c : pfx)
        if (!std::isalnum((unsigned char)c)) {
            out.errcode = "FIELDS_REQUIRED"; out.msg = "prefix 只能包含字母数字"; return out;
        }

    auto guard = m_pool.acquire(5000);
    if (!guard) { out.errcode = "DB_ERROR"; out.msg = "db busy"; return out; }
    MYSQL *db = guard->get();

    static const std::string ALPHA = "ABCDEFGHJKMNPQRSTUVWXYZ23456789"; // 去掉 0O1IL 易混字符
    static std::mt19937_64 rng(
        std::random_device{}() ^ (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count());

    char q[1024]; char eesc[64];
    for (uint64_t i = 0; i < count; i++) {
        bool inserted = false;
        for (int attempt = 0; attempt < 3 && !inserted; attempt++) {
            std::string code = pfx + "-";
            for (int k = 0; k < 12; k++)
                code += ALPHA[rng() % ALPHA.size()];
            uint64_t id = Snowflake::generate();
            std::string expPart = expDays > 0
                ? "DATE_ADD(NOW(), INTERVAL " + std::to_string(expDays) + " DAY)" : "NULL";
            mysql_real_escape_string(db, eesc, code.c_str(), code.size());
            snprintf(q, sizeof(q),
                "INSERT INTO redeem_codes (id, code, days, max_uses, used_count, is_active, expires_at) "
                "VALUES (%llu,'%s',%llu,%llu,0,1,%s)",
                id, eesc, days, maxUses, expPart.c_str());
            if (mysql_query(db, q) == 0) { inserted = true; out.codes.push_back(code); }
        }
        if (!inserted) {
            out.errcode = "GEN_FAILED";
            out.msg = "生成失败: 随机码碰撞, 请重试 (已生成 "
                    + std::to_string(out.codes.size()) + " 个)";
            return out;
        }
    }

    out.ok = true;
    return out;
}

RedeemStore::ListResult RedeemStore::list(uint64_t page) {
    ListResult out;
    if (page < 1) page = 1;
    uint64_t offset = (page - 1) * 50;

    auto guard = m_pool.acquire(5000);
    if (!guard) { out.errcode = "DB_ERROR"; out.msg = "db busy"; return out; }
    MYSQL *db = guard->get();
    char q[512];

    snprintf(q, sizeof(q),
        "SELECT id, code, days, max_uses, used_count, is_active, expires_at, created_at "
        "FROM redeem_codes ORDER BY created_at DESC, id DESC LIMIT 50 OFFSET %llu", offset);
    mysql_query(db, q);
    MYSQL_RES *res = mysql_store_result(db);
    MYSQL_ROW row;
    while (res && (row = mysql_fetch_row(res))) {
        CodeRow r;
        r.code      = row[1] ? row[1] : "";
        r.days      = row[2] ? std::stoull(row[2]) : 0;
        r.maxUses   = row[3] ? std::stoull(row[3]) : 0;
        r.usedCount = row[4] ? std::stoull(row[4]) : 0;
        r.isActive  = row[5] ? std::stoi(row[5]) : 0;
        r.expiresAt = row[6] ? row[6] : "";
        r.createdAt = row[7] ? row[7] : "";
        out.rows.push_back(std::move(r));
    }
    if (res) mysql_free_result(res);

    // 汇总: 总数 / 生效数 / 总使用次数
    snprintf(q, sizeof(q),
        "SELECT COUNT(*), COALESCE(SUM(is_active=1),0), COALESCE(SUM(used_count),0) FROM redeem_codes");
    mysql_query(db, q);
    MYSQL_RES *res2 = mysql_store_result(db);
    if (res2) {
        MYSQL_ROW r2 = mysql_fetch_row(res2);
        if (r2) {
            out.total     = r2[0] ? std::stoull(r2[0]) : 0;
            out.active    = r2[1] ? std::stoull(r2[1]) : 0;
            out.totalUsed = r2[2] ? std::stoull(r2[2]) : 0;
        }
        mysql_free_result(res2);
    }

    out.ok = true;
    return out;
}

RedeemStore::DisableResult RedeemStore::disable(const std::string &rawCode) {
    DisableResult out;

    // 码规范化: trim + 大写 (与兑换入口一致)
    out.code = rawCode;
    size_t bs = out.code.find_first_not_of(" \t"), be = out.code.find_last_not_of(" \t");
    out.code = (bs == std::string::npos) ? "" : out.code.substr(bs, be - bs + 1);
    for (char &c : out.code) c = (char)std::toupper((unsigned char)c);
    if (out.code.empty()) {
        out.errcode = "FIELDS_REQUIRED"; out.msg = "code 必填"; return out;
    }

    auto guard = m_pool.acquire(5000);
    if (!guard) { out.errcode = "DB_ERROR"; out.msg = "db busy"; return out; }
    MYSQL *db = guard->get();
    char q[512]; char eesc[64];
    mysql_real_escape_string(db, eesc, out.code.c_str(), out.code.size());
    snprintf(q, sizeof(q), "UPDATE redeem_codes SET is_active=0 WHERE code='%s'", eesc);
    if (mysql_query(db, q) != 0) {
        out.errcode = "DB_ERROR"; out.msg = mysql_error(db); return out;
    }
    if (mysql_affected_rows(db) == 0) {
        out.notFound = true;
        out.errcode = "NOT_FOUND"; out.msg = "兑换码不存在: " + out.code;
        return out;
    }

    out.ok = true;
    return out;
}
