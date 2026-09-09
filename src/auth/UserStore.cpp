#include "UserStore.h"
#include "../core/database/DbPool.h"
#include "PasswordHash.h"
#include "../common/Snowflake.h"
#include <mysql/mysql.h>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <cstdlib>

// ── 辅助：预处理语句 RAII ──────────────────────────────
struct StmtGuard {
    MYSQL_STMT* stmt;
    ~StmtGuard() { if (stmt) mysql_stmt_close(stmt); }
    operator MYSQL_STMT*() const { return stmt; }
};

// ── 辅助：MySQL datetime → 兼容旧格式 ─────────────────
static std::string fmtTime(const char* mysqlDt) {
    // "2024-06-26 14:02:03" → "2024-06-26T14:02:03Z"
    if (!mysqlDt) return {};
    std::string s(mysqlDt);
    if (s.size() >= 10) s[10] = 'T';
    s += 'Z';
    return s;
}

// ═══════════════════════════════════════════════════════
//  exists
// ═══════════════════════════════════════════════════════
bool UserStore::exists(const std::string& email) const {
    auto guard = m_pool.acquire(5000);
    if (!guard) return false;
    MYSQL* db = guard->get();

    StmtGuard stmt{mysql_stmt_init(db)};
    const char* sql = "SELECT COUNT(*) FROM users WHERE LOWER(email) = LOWER(?)";
    mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql));

    MYSQL_BIND bind[1] = {};
    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = (void*)email.c_str();
    bind[0].buffer_length = (unsigned long)email.size();
    mysql_stmt_bind_param(stmt, bind);

    mysql_stmt_execute(stmt);

    long long cnt = 0;
    MYSQL_BIND result[1] = {};
    result[0].buffer_type = MYSQL_TYPE_LONGLONG;
    result[0].buffer = &cnt;
    mysql_stmt_bind_result(stmt, result);
    mysql_stmt_store_result(stmt);

    bool ok = (mysql_stmt_fetch(stmt) == 0 && cnt > 0);
    return ok;
}

// ═══════════════════════════════════════════════════════
//  findByEmail
// ═══════════════════════════════════════════════════════
std::optional<UserStore::UserInfo>
UserStore::findByEmail(const std::string& email) const {
    auto guard = m_pool.acquire(5000);
    MYSQL* db = guard->get();

    StmtGuard stmt{mysql_stmt_init(db)};
    const char* sql =
        "SELECT id, email, username, created_at, subscription_expires, status "
        "FROM users WHERE LOWER(email) = LOWER(?)";
    int prepRc = mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql));
    if (prepRc != 0) {
        return std::nullopt;
    }

    MYSQL_BIND bind[1] = {};
    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = (void*)email.c_str();
    bind[0].buffer_length = (unsigned long)email.size();
    mysql_stmt_bind_param(stmt, bind);

    int execRc = mysql_stmt_execute(stmt);
    if (execRc != 0) {
        return std::nullopt;
    }

    unsigned long long id = 0;
    char dbEmail[256] = {}, dbName[256] = {}, dbCreated[32] = {};
    unsigned long lenEmail = 0, lenName = 0, lenCreated = 0;
    bool isNullEmail = 0, isNullName = 0, isNullCreated = 0;

    char dbSubExp[32] = {};
    unsigned long lenSubExp = 0;
    bool isNullSubExp = 0;
    char dbStatus = 0;
    bool isNullStatus = 0;

    MYSQL_BIND result[6] = {};
    result[0].buffer_type = MYSQL_TYPE_LONGLONG;
    result[0].buffer = &id;
    result[1].buffer_type = MYSQL_TYPE_STRING;
    result[1].buffer = dbEmail; result[1].buffer_length = sizeof(dbEmail) - 1;
    result[1].length = &lenEmail; result[1].is_null = &isNullEmail;
    result[2].buffer_type = MYSQL_TYPE_STRING;
    result[2].buffer = dbName; result[2].buffer_length = sizeof(dbName) - 1;
    result[2].length = &lenName; result[2].is_null = &isNullName;
    result[3].buffer_type = MYSQL_TYPE_STRING;
    result[3].buffer = dbCreated; result[3].buffer_length = sizeof(dbCreated) - 1;
    result[3].length = &lenCreated; result[3].is_null = &isNullCreated;
    result[4].buffer_type = MYSQL_TYPE_STRING;
    result[4].buffer = dbSubExp; result[4].buffer_length = sizeof(dbSubExp) - 1;
    result[4].length = &lenSubExp; result[4].is_null = &isNullSubExp;
    result[5].buffer_type = MYSQL_TYPE_TINY;
    result[5].buffer = &dbStatus; result[5].is_null = &isNullStatus;
    mysql_stmt_bind_result(stmt, result);
    int storeRc = mysql_stmt_store_result(stmt);

    int fetchRc = mysql_stmt_fetch(stmt);
    if (fetchRc != 0) {
        return std::nullopt;
    }

    dbEmail[lenEmail] = '\0';
    dbName[lenName] = '\0';
    dbCreated[lenCreated] = '\0';
    if (!isNullSubExp) dbSubExp[lenSubExp] = '\0';

    UserInfo info;
    info.id        = id;
    info.email     = dbEmail;
    info.username  = dbName;
    info.createdAt = fmtTime(dbCreated);
    if (!isNullSubExp) info.subscription_expires = dbSubExp;
    if (!isNullStatus) info.status = dbStatus;

    // 查 OAuth 绑定（新作用域，让 stmt 析构先释放）
    {
        MYSQL_STMT* stmt2 = mysql_stmt_init(db);
        const char* sql2 =
            "SELECT provider, provider_id FROM oauth_links WHERE user_id = ?";
        mysql_stmt_prepare(stmt2, sql2, (unsigned long)strlen(sql2));

        MYSQL_BIND bind2[1] = {};
        bind2[0].buffer_type = MYSQL_TYPE_LONGLONG;
        bind2[0].buffer = &id;
        mysql_stmt_bind_param(stmt2, bind2);
        mysql_stmt_execute(stmt2);

        char prov[64] = {}, provId[256] = {};
        unsigned long lenProv = 0, lenProvId = 0;
        MYSQL_BIND res2[2] = {};
        res2[0].buffer_type = MYSQL_TYPE_STRING;
        res2[0].buffer = prov; res2[0].buffer_length = sizeof(prov) - 1;
        res2[0].length = &lenProv;
        res2[1].buffer_type = MYSQL_TYPE_STRING;
        res2[1].buffer = provId; res2[1].buffer_length = sizeof(provId) - 1;
        res2[1].length = &lenProvId;
        mysql_stmt_bind_result(stmt2, res2);
        mysql_stmt_store_result(stmt2);

        while (mysql_stmt_fetch(stmt2) == 0) {
            prov[lenProv] = '\0';
            provId[lenProvId] = '\0';
            info.oauthLinks[prov] = provId;
        }
        mysql_stmt_close(stmt2);
    }

    return info;
}

// ═══════════════════════════════════════════════════════
//  create (注册)
// ═══════════════════════════════════════════════════════
bool UserStore::create(const std::string& email,
                        const std::string& password,
                        const std::string& username) {
    auto guard = m_pool.acquire(5000);
    if (!guard) return false;
    MYSQL* db = guard->get();

    std::string salt = PasswordHash::generateSalt();       // 独立盐
    std::string name = username.empty()
        ? email.substr(0, email.find('@')) : username;

    uint64_t uid = Snowflake::generate();                  // 雪花 ID

    // 计算 PBKDF2（公式: 盐 + 密码 + uid）
    std::string hash = PasswordHash::hash(salt, password, uid);

    // INSERT（一次性写入，无需二次 UPDATE）
    {
        StmtGuard stmt{mysql_stmt_init(db)};
        const char* sql =
            "INSERT INTO users (id, email, username, salt, password_hash) "
            "VALUES (?, ?, ?, ?, ?)";
        mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql));

        MYSQL_BIND bind[5] = {};
        bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
        bind[0].buffer = &uid;
        bind[1].buffer_type = MYSQL_TYPE_STRING;
        bind[1].buffer = (void*)email.c_str();
        bind[1].buffer_length = (unsigned long)email.size();
        bind[2].buffer_type = MYSQL_TYPE_STRING;
        bind[2].buffer = (void*)name.c_str();
        bind[2].buffer_length = (unsigned long)name.size();
        bind[3].buffer_type = MYSQL_TYPE_STRING;
        bind[3].buffer = (void*)salt.c_str();
        bind[3].buffer_length = (unsigned long)salt.size();
        bind[4].buffer_type = MYSQL_TYPE_STRING;
        bind[4].buffer = (void*)hash.c_str();
        bind[4].buffer_length = (unsigned long)hash.size();
        mysql_stmt_bind_param(stmt, bind);

        if (mysql_stmt_execute(stmt) != 0) {
            if (mysql_stmt_errno(stmt) == 1062) return false;
            return false;
        }
    }
    return true;
}

// ═══════════════════════════════════════════════════════
//  updatePassword
// ═══════════════════════════════════════════════════════
bool UserStore::updatePassword(const std::string& email,
                                 const std::string& newPassword) {
    auto guard = m_pool.acquire(5000);
    if (!guard) return false;
    MYSQL* db = guard->get();

    // 查出 uid 和 salt
    uint64_t uid = 0;
    char salt[65] = {};
    unsigned long saltLen = 0;
    {
        StmtGuard stmt{mysql_stmt_init(db)};
        const char* sql =
            "SELECT id, salt FROM users WHERE LOWER(email) = LOWER(?)";
        mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql));

        MYSQL_BIND bind[1] = {};
        bind[0].buffer_type = MYSQL_TYPE_STRING;
        bind[0].buffer = (void*)email.c_str();
        bind[0].buffer_length = (unsigned long)email.size();
        mysql_stmt_bind_param(stmt, bind);
        mysql_stmt_execute(stmt);

        MYSQL_BIND res[2] = {};
        res[0].buffer_type = MYSQL_TYPE_LONGLONG;
        res[0].buffer = &uid;
        res[1].buffer_type = MYSQL_TYPE_STRING;
        res[1].buffer = salt; res[1].buffer_length = sizeof(salt) - 1;
        res[1].length = &saltLen;
        mysql_stmt_bind_result(stmt, res);
        mysql_stmt_store_result(stmt);

        if (mysql_stmt_fetch(stmt) != 0) return false;  // 用户不存在
        salt[saltLen] = '\0';
    }

    // OAuth 用户首次设密码：生成新盐；已有密码则保留原盐
    std::string newSalt(salt);
    if (newSalt.empty() || newSalt.size() < 16) {
        newSalt = PasswordHash::generateSalt();
        // 同时更新盐值
        StmtGuard us{mysql_stmt_init(db)};
        const char* usql = "UPDATE users SET salt = ? WHERE id = ?";
        mysql_stmt_prepare(us, usql, (unsigned long)strlen(usql));
        MYSQL_BIND ub[2] = {};
        ub[0].buffer_type = MYSQL_TYPE_STRING;
        ub[0].buffer = (void*)newSalt.c_str();
        ub[0].buffer_length = (unsigned long)newSalt.size();
        ub[1].buffer_type = MYSQL_TYPE_LONGLONG;
        ub[1].buffer = &uid;
        mysql_stmt_bind_param(us, ub);
        mysql_stmt_execute(us);
    }

    // 重算哈希
    std::string hash = PasswordHash::hash(newSalt, newPassword, uid);

    // 更新
    StmtGuard stmt{mysql_stmt_init(db)};
    const char* sql = "UPDATE users SET password_hash = ? WHERE id = ?";
    mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql));

    MYSQL_BIND bind[2] = {};
    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = (void*)hash.c_str();
    bind[0].buffer_length = (unsigned long)hash.size();
    bind[1].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[1].buffer = &uid;
    mysql_stmt_bind_param(stmt, bind);

    return mysql_stmt_execute(stmt) == 0;
}

// ═══════════════════════════════════════════════════════
//  verifyPassword
// ═══════════════════════════════════════════════════════
bool UserStore::verifyPassword(const std::string& email,
                                 const std::string& password) const {
    auto guard = m_pool.acquire(5000);
    if (!guard) return false;
    MYSQL* db = guard->get();

    // 查出 uid, salt, password_hash
    StmtGuard stmt{mysql_stmt_init(db)};
    const char* sql =
        "SELECT id, salt, password_hash FROM users WHERE LOWER(email) = LOWER(?)";
    mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql));

    MYSQL_BIND bind[1] = {};
    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = (void*)email.c_str();
    bind[0].buffer_length = (unsigned long)email.size();
    mysql_stmt_bind_param(stmt, bind);
    mysql_stmt_execute(stmt);

    uint64_t uid = 0;
    char salt[65] = {}, hash[65] = {};
    unsigned long saltLen = 0, hashLen = 0;

    MYSQL_BIND res[3] = {};
    res[0].buffer_type = MYSQL_TYPE_LONGLONG;
    res[0].buffer = &uid;
    res[1].buffer_type = MYSQL_TYPE_STRING;
    res[1].buffer = salt; res[1].buffer_length = sizeof(salt) - 1;
    res[1].length = &saltLen;
    res[2].buffer_type = MYSQL_TYPE_STRING;
    res[2].buffer = hash; res[2].buffer_length = sizeof(hash) - 1;
    res[2].length = &hashLen;
    mysql_stmt_bind_result(stmt, res);
    mysql_stmt_store_result(stmt);

    if (mysql_stmt_fetch(stmt) != 0) {
        return false;
    }

    salt[saltLen] = '\0';
    hash[hashLen] = '\0';

    // 同样公式重算并比较
    return PasswordHash::verify(salt, password, uid, hash);
}

// ═══════════════════════════════════════════════════════
//  findAuthByEmail (登录专用: exists+verifyPassword+findByEmail 合并为一次查询)
// ═══════════════════════════════════════════════════════
std::optional<UserStore::AuthCred>
UserStore::findAuthByEmail(const std::string& email) const {
    auto guard = m_pool.acquire(5000);
    if (!guard) return std::nullopt;
    MYSQL* db = guard->get();

    StmtGuard stmt{mysql_stmt_init(db)};
    const char* sql =
        "SELECT id, email, username, created_at, subscription_expires, salt, password_hash, status "
        "FROM users WHERE LOWER(email) = LOWER(?)";
    if (mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql)) != 0)
        return std::nullopt;

    MYSQL_BIND bind[1] = {};
    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = (void*)email.c_str();
    bind[0].buffer_length = (unsigned long)email.size();
    mysql_stmt_bind_param(stmt, bind);

    if (mysql_stmt_execute(stmt) != 0)
        return std::nullopt;

    unsigned long long id = 0;
    char dbEmail[256] = {}, dbName[256] = {}, dbCreated[32] = {};
    char dbSalt[128] = {}, dbHash[128] = {};
    unsigned long lenEmail = 0, lenName = 0, lenCreated = 0, lenSalt = 0, lenHash = 0;
    char dbSubExp[32] = {};
    unsigned long lenSubExp = 0;
    bool isNullSubExp = 0;
    char dbStatus = 0;
    bool isNullStatus = 0;

    MYSQL_BIND result[8] = {};
    result[0].buffer_type = MYSQL_TYPE_LONGLONG;
    result[0].buffer = &id;
    result[1].buffer_type = MYSQL_TYPE_STRING;
    result[1].buffer = dbEmail; result[1].buffer_length = sizeof(dbEmail) - 1;
    result[1].length = &lenEmail;
    result[2].buffer_type = MYSQL_TYPE_STRING;
    result[2].buffer = dbName; result[2].buffer_length = sizeof(dbName) - 1;
    result[2].length = &lenName;
    result[3].buffer_type = MYSQL_TYPE_STRING;
    result[3].buffer = dbCreated; result[3].buffer_length = sizeof(dbCreated) - 1;
    result[3].length = &lenCreated;
    result[4].buffer_type = MYSQL_TYPE_STRING;
    result[4].buffer = dbSubExp; result[4].buffer_length = sizeof(dbSubExp) - 1;
    result[4].length = &lenSubExp; result[4].is_null = &isNullSubExp;
    result[5].buffer_type = MYSQL_TYPE_STRING;
    result[5].buffer = dbSalt; result[5].buffer_length = sizeof(dbSalt) - 1;
    result[5].length = &lenSalt;
    result[6].buffer_type = MYSQL_TYPE_STRING;
    result[6].buffer = dbHash; result[6].buffer_length = sizeof(dbHash) - 1;
    result[6].length = &lenHash;
    result[7].buffer_type = MYSQL_TYPE_TINY;
    result[7].buffer = &dbStatus; result[7].is_null = &isNullStatus;
    mysql_stmt_bind_result(stmt, result);
    mysql_stmt_store_result(stmt);

    if (mysql_stmt_fetch(stmt) != 0)
        return std::nullopt;

    dbEmail[lenEmail] = '\0';
    dbName[lenName] = '\0';
    dbCreated[lenCreated] = '\0';
    dbSalt[lenSalt] = '\0';
    dbHash[lenHash] = '\0';
    if (!isNullSubExp) dbSubExp[lenSubExp] = '\0';

    AuthCred cred;
    cred.id = id;
    cred.email = dbEmail;
    cred.username = dbName;
    cred.salt = dbSalt;
    cred.passwordHash = dbHash;
    cred.createdAt = fmtTime(dbCreated);
    if (!isNullSubExp) cred.subscription_expires = dbSubExp;
    if (!isNullStatus) cred.status = dbStatus;
    return cred;
}

// ═══════════════════════════════════════════════════════
//  findByOAuth
// ═══════════════════════════════════════════════════════
std::optional<UserStore::UserInfo>
UserStore::findByOAuth(const std::string& provider,
                        const std::string& providerId) const {
    auto guard = m_pool.acquire(5000);
    if (!guard) return std::nullopt;
    MYSQL* db = guard->get();

    StmtGuard stmt{mysql_stmt_init(db)};
    const char* sql =
        "SELECT u.id, u.email, u.username, u.created_at "
        "FROM users u JOIN oauth_links o ON u.id = o.user_id "
        "WHERE o.provider = ? AND o.provider_id = ?";
    mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql));

    MYSQL_BIND bind[2] = {};
    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = (void*)provider.c_str();
    bind[0].buffer_length = (unsigned long)provider.size();
    bind[1].buffer_type = MYSQL_TYPE_STRING;
    bind[1].buffer = (void*)providerId.c_str();
    bind[1].buffer_length = (unsigned long)providerId.size();
    mysql_stmt_bind_param(stmt, bind);
    mysql_stmt_execute(stmt);

    unsigned long long id = 0;
    char dbEmail[256] = {}, dbName[256] = {}, dbCreated[32] = {};
    unsigned long lenEmail = 0, lenName = 0, lenCreated = 0;

    MYSQL_BIND res[4] = {};
    res[0].buffer_type = MYSQL_TYPE_LONGLONG;
    res[0].buffer = &id;
    res[1].buffer_type = MYSQL_TYPE_STRING;
    res[1].buffer = dbEmail; res[1].buffer_length = sizeof(dbEmail) - 1;
    res[1].length = &lenEmail;
    res[2].buffer_type = MYSQL_TYPE_STRING;
    res[2].buffer = dbName; res[2].buffer_length = sizeof(dbName) - 1;
    res[2].length = &lenName;
    res[3].buffer_type = MYSQL_TYPE_STRING;
    res[3].buffer = dbCreated; res[3].buffer_length = sizeof(dbCreated) - 1;
    res[3].length = &lenCreated;
    mysql_stmt_bind_result(stmt, res);
    mysql_stmt_store_result(stmt);

    if (mysql_stmt_fetch(stmt) != 0) {
        return std::nullopt;
    }

    dbEmail[lenEmail] = '\0';
    dbName[lenName] = '\0';
    dbCreated[lenCreated] = '\0';

    UserInfo info;
    info.id        = id;
    info.email     = dbEmail;
    info.username  = dbName;
    info.createdAt = fmtTime(dbCreated);
    // 不重新查 oauth_links（调用方不需要完整列表即可工作）
    info.oauthLinks[provider] = providerId;
    return info;
}

// ═══════════════════════════════════════════════════════
//  linkOAuth
// ═══════════════════════════════════════════════════════
bool UserStore::linkOAuth(const std::string& email,
                           const std::string& provider,
                           const std::string& providerId) {
    auto guard = m_pool.acquire(5000);
    if (!guard) return false;
    MYSQL* db = guard->get();

    // 查 user_id
    StmtGuard stmt{mysql_stmt_init(db)};
    const char* sql = "SELECT id FROM users WHERE LOWER(email) = LOWER(?)";
    mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql));

    MYSQL_BIND bind[1] = {};
    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = (void*)email.c_str();
    bind[0].buffer_length = (unsigned long)email.size();
    mysql_stmt_bind_param(stmt, bind);
    mysql_stmt_execute(stmt);

    uint64_t uid = 0;
    MYSQL_BIND res[1] = {};
    res[0].buffer_type = MYSQL_TYPE_LONGLONG;
    res[0].buffer = &uid;
    mysql_stmt_bind_result(stmt, res);
    mysql_stmt_store_result(stmt);

    if (mysql_stmt_fetch(stmt) != 0) return false;
    // stmt 由 StmtGuard 自动释放（作用域结束时）

    // INSERT 绑定（ER_DUP_ENTRY = 已绑定，算成功）
    uint64_t linkId = Snowflake::generate();
    MYSQL_STMT* stmt2 = mysql_stmt_init(db);
    const char* sql2 =
        "INSERT INTO oauth_links (id, user_id, provider, provider_id) "
        "VALUES (?, ?, ?, ?)";
    mysql_stmt_prepare(stmt2, sql2, (unsigned long)strlen(sql2));

    MYSQL_BIND bind2[4] = {};
    bind2[0].buffer_type = MYSQL_TYPE_LONGLONG;
    bind2[0].buffer = &linkId;
    bind2[1].buffer_type = MYSQL_TYPE_LONGLONG;
    bind2[1].buffer = &uid;
    bind2[2].buffer_type = MYSQL_TYPE_STRING;
    bind2[2].buffer = (void*)provider.c_str();
    bind2[2].buffer_length = (unsigned long)provider.size();
    bind2[3].buffer_type = MYSQL_TYPE_STRING;
    bind2[3].buffer = (void*)providerId.c_str();
    bind2[3].buffer_length = (unsigned long)providerId.size();
    mysql_stmt_bind_param(stmt2, bind2);

    int err = mysql_stmt_execute(stmt2);
    mysql_stmt_close(stmt2);

    return err == 0 || mysql_errno(db) == 1062;  // 1062 = 已存在
}

// ═══════════════════════════════════════════════════════
//  createFromOAuth
// ═══════════════════════════════════════════════════════
bool UserStore::createFromOAuth(const std::string& email,
                                 const std::string& name,
                                 const std::string& provider,
                                 const std::string& providerId) {
    auto guard = m_pool.acquire(5000);
    if (!guard) return false;
    MYSQL* db = guard->get();

    // 先看邮箱是否已存在
    {
        StmtGuard stmt{mysql_stmt_init(db)};
        const char* sql =
            "SELECT COUNT(*) FROM users WHERE LOWER(email) = LOWER(?)";
        mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql));
        MYSQL_BIND b[1] = {};
        b[0].buffer_type = MYSQL_TYPE_STRING;
        b[0].buffer = (void*)email.c_str();
        b[0].buffer_length = (unsigned long)email.size();
        mysql_stmt_bind_param(stmt, b);
        mysql_stmt_execute(stmt);
        long long cnt = 0;
        MYSQL_BIND r[1] = {};
        r[0].buffer_type = MYSQL_TYPE_LONGLONG;
        r[0].buffer = &cnt;
        mysql_stmt_bind_result(stmt, r);
        mysql_stmt_store_result(stmt);
        mysql_stmt_fetch(stmt);

        if (cnt > 0)
            return linkOAuth(email, provider, providerId);  // 已有用户，只绑定
    }

    // 新建用户（OAuth 用户无密码）
    {
        uint64_t uid = Snowflake::generate();              // 雪花 ID

        StmtGuard stmt{mysql_stmt_init(db)};
        const char* sql =
            "INSERT INTO users (id, email, username, salt, password_hash) "
            "VALUES (?, ?, ?, '', '')";
        mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql));

        MYSQL_BIND b[3] = {};
        b[0].buffer_type = MYSQL_TYPE_LONGLONG;
        b[0].buffer = &uid;
        b[1].buffer_type = MYSQL_TYPE_STRING;
        b[1].buffer = (void*)email.c_str();
        b[1].buffer_length = (unsigned long)email.size();
        b[2].buffer_type = MYSQL_TYPE_STRING;
        b[2].buffer = (void*)name.c_str();
        b[2].buffer_length = (unsigned long)name.size();
        mysql_stmt_bind_param(stmt, b);

        if (mysql_stmt_execute(stmt) != 0) {
            if (mysql_stmt_errno(stmt) == 1062)
                return linkOAuth(email, provider, providerId);
            return false;
        }

        // 绑定 OAuth
        uint64_t linkId = Snowflake::generate();
        MYSQL_STMT* stmt2 = mysql_stmt_init(db);
        const char* sql2 =
            "INSERT INTO oauth_links (id, user_id, provider, provider_id) "
            "VALUES (?, ?, ?, ?)";
        mysql_stmt_prepare(stmt2, sql2, (unsigned long)strlen(sql2));

        MYSQL_BIND b2[4] = {};
        b2[0].buffer_type = MYSQL_TYPE_LONGLONG;
        b2[0].buffer = &linkId;
        b2[1].buffer_type = MYSQL_TYPE_LONGLONG;
        b2[1].buffer = &uid;
        b2[2].buffer_type = MYSQL_TYPE_STRING;
        b2[2].buffer = (void*)provider.c_str();
        b2[2].buffer_length = (unsigned long)provider.size();
        b2[3].buffer_type = MYSQL_TYPE_STRING;
        b2[3].buffer = (void*)providerId.c_str();
        b2[3].buffer_length = (unsigned long)providerId.size();
        mysql_stmt_bind_param(stmt2, b2);

        int err = mysql_stmt_execute(stmt2);
        mysql_stmt_close(stmt2);
        return err == 0 || mysql_errno(db) == 1062;
    }
}

// ═══════════════════════════════════════════════════════
//  count
// ═══════════════════════════════════════════════════════
// isBanned: 封禁查询 - 单一数据源 MySQL users.status, 无缓存,
// 解封(UPDATE status=0)必然立即生效, 不存在"解封了解不了"的残留状态
bool UserStore::isBanned(uint64_t id) const {
    auto guard = m_pool.acquire(3000);
    if (!guard) return false;   // DB busy -> fail-open
    MYSQL* db = guard->get();
    char q[128];
    snprintf(q, sizeof(q),
        "SELECT 1 FROM users WHERE id=%llu AND status=1 LIMIT 1",
        (unsigned long long)id);
    if (mysql_query(db, q) != 0) return false;
    MYSQL_RES* res = mysql_store_result(db);
    if (!res) return false;
    bool banned = (mysql_num_rows(res) > 0);
    mysql_free_result(res);
    return banned;
}

int UserStore::count() const {
    auto guard = m_pool.acquire(5000);
    if (!guard) return 0;
    MYSQL* db = guard->get();

    if (mysql_query(db, "SELECT COUNT(*) FROM users") != 0) return 0;
    MYSQL_RES* res = mysql_store_result(db);
    if (!res) return 0;
    MYSQL_ROW row = mysql_fetch_row(res);
    int n = row ? atoi(row[0]) : 0;
    mysql_free_result(res);
    return n;
}

// ═══════════════════════════════════════════════════════
//  findById
// ═══════════════════════════════════════════════════════
std::optional<UserStore::UserInfo> UserStore::findById(uint64_t id) const {
    auto guard = m_pool.acquire(5000);
    if (!guard) return {};
    MYSQL* db = guard->get();

    StmtGuard stmt{mysql_stmt_init(db)};
    const char* sql = "SELECT id, email, username, created_at FROM users WHERE id = ?";
    mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql));
    MYSQL_BIND b[1] = {};
    b[0].buffer_type = MYSQL_TYPE_LONGLONG; b[0].buffer = &id;
    mysql_stmt_bind_param(stmt, b);
    mysql_stmt_execute(stmt);

    uint64_t uid = 0; char email[256]={}, name[256]={}, ca[32]={};
    unsigned long el=0, nl=0, cl=0;
    MYSQL_BIND r[4] = {};
    r[0].buffer_type = MYSQL_TYPE_LONGLONG; r[0].buffer = &uid;
    r[1].buffer_type = MYSQL_TYPE_STRING; r[1].buffer = email; r[1].buffer_length = 255; r[1].length = &el;
    r[2].buffer_type = MYSQL_TYPE_STRING; r[2].buffer = name; r[2].buffer_length = 255; r[2].length = &nl;
    r[3].buffer_type = MYSQL_TYPE_STRING; r[3].buffer = ca; r[3].buffer_length = 31; r[3].length = &cl;
    mysql_stmt_bind_result(stmt, r);
    mysql_stmt_store_result(stmt);
    if (mysql_stmt_fetch(stmt) != 0) return {};
    email[el]='\0'; name[nl]='\0'; ca[cl]='\0';
    return UserInfo{uid, email, name, "", ca};
}

// ═══════════════════════════════════════════════════════
//  listOAuthLinks — 返回 provider → {provider_id, username}
// ═══════════════════════════════════════════════════════
std::vector<std::tuple<std::string,std::string,std::string>>
UserStore::listOAuthLinks(const std::string &email) const {
    std::vector<std::tuple<std::string,std::string,std::string>> links;
    auto guard = m_pool.acquire(5000);
    MYSQL* db = guard->get();

    StmtGuard stmt{mysql_stmt_init(db)};
    const char* sql =
        "SELECT ol.provider, ol.provider_id, '' "
        "FROM oauth_links ol "
        "JOIN users u ON ol.user_id = u.id "
        "WHERE LOWER(u.email) = LOWER(?)";
    mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql));
    MYSQL_BIND b[1] = {};
    b[0].buffer_type = MYSQL_TYPE_STRING;
    b[0].buffer = (void*)email.c_str();
    b[0].buffer_length = (unsigned long)email.size();
    mysql_stmt_bind_param(stmt, b);
    mysql_stmt_execute(stmt);
    mysql_stmt_store_result(stmt);

    char prov[64]={}, pid[256]={}, puid[256]={};
    unsigned long pl=0, il=0, ul=0;
    MYSQL_BIND r[3] = {};
    r[0].buffer_type = MYSQL_TYPE_STRING; r[0].buffer = prov; r[0].buffer_length = 63; r[0].length = &pl;
    r[1].buffer_type = MYSQL_TYPE_STRING; r[1].buffer = pid; r[1].buffer_length = 255; r[1].length = &il;
    r[2].buffer_type = MYSQL_TYPE_STRING; r[2].buffer = puid; r[2].buffer_length = 255; r[2].length = &ul;
    mysql_stmt_bind_result(stmt, r);
    while (mysql_stmt_fetch(stmt) == 0) {
        prov[pl]='\0'; pid[il]='\0'; puid[ul]='\0';
        links.push_back({prov, pid, puid});
    }
    return links;
}

// ═══════════════════════════════════════════════════════
//  unlinkOAuth
// ═══════════════════════════════════════════════════════
bool UserStore::unlinkOAuth(const std::string &email, const std::string &provider) {
    auto guard = m_pool.acquire(5000);
    if (!guard) return false;
    MYSQL* db = guard->get();

    auto user = findByEmail(email);
    if (!user) return false;

    StmtGuard stmt{mysql_stmt_init(db)};
    const char* sql = "DELETE FROM oauth_links WHERE user_id = ? AND provider = ?";
    mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql));
    MYSQL_BIND b[2] = {};
    b[0].buffer_type = MYSQL_TYPE_LONGLONG; b[0].buffer = &user->id;
    b[1].buffer_type = MYSQL_TYPE_STRING;
    b[1].buffer = (void*)provider.c_str();
    b[1].buffer_length = (unsigned long)provider.size();
    mysql_stmt_bind_param(stmt, b);
    return mysql_stmt_execute(stmt) == 0;
}

// ═══════════════════════════════════════════════════════
//  balanceOf (2026-09-06 收敛自 WalletRoutes 直连 SQL)
// ═══════════════════════════════════════════════════════
unsigned long long UserStore::balanceOf(uint64_t id) const {
    auto guard = m_pool.acquire(5000);
    if (!guard) return 0;
    MYSQL* db = guard->get();

    StmtGuard stmt{mysql_stmt_init(db)};
    const char* sql = "SELECT balance FROM users WHERE id=?";
    mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql));
    MYSQL_BIND b[1] = {};
    b[0].buffer_type = MYSQL_TYPE_LONGLONG; b[0].buffer = &id;
    mysql_stmt_bind_param(stmt, b);
    mysql_stmt_execute(stmt);
    mysql_stmt_store_result(stmt);

    unsigned long long bal = 0;
    MYSQL_BIND r[1] = {};
    r[0].buffer_type = MYSQL_TYPE_LONGLONG; r[0].buffer = &bal;
    mysql_stmt_bind_result(stmt, r);
    mysql_stmt_fetch(stmt);
    return bal;
}

// ═══════════════════════════════════════════════════════
//  profileOf (2026-09-09 收敛自 AuthRoutes profile 直连 SQL)
//  balance 按字符串读: 列类型为 DECIMAL, 保留原始形态与旧实现一致
// ═══════════════════════════════════════════════════════
std::optional<UserStore::Profile> UserStore::profileOf(uint64_t id) const {
    auto guard = m_pool.acquire(5000);
    if (!guard) return std::nullopt;
    MYSQL* db = guard->get();

    StmtGuard stmt{mysql_stmt_init(db)};
    const char* sql =
        "SELECT email, username, COALESCE(avatar_url,''), provider, "
        "UNIX_TIMESTAMP(GREATEST(COALESCE(subscription_expires, NOW()), NOW())), "
        "balance FROM users WHERE id=?";
    if (mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql)) != 0)
        return std::nullopt;

    MYSQL_BIND b[1] = {};
    b[0].buffer_type = MYSQL_TYPE_LONGLONG; b[0].buffer = &id;
    mysql_stmt_bind_param(stmt, b);
    if (mysql_stmt_execute(stmt) != 0) return std::nullopt;

    char dbEmail[256] = {}, dbName[256] = {}, dbAvatar[1024] = {}, dbProv[64] = {}, dbBal[64] = {};
    unsigned long lenEmail = 0, lenName = 0, lenAvatar = 0, lenProv = 0, lenBal = 0;
    bool nullEmail = 0, nullName = 0, nullAvatar = 0, nullProv = 0, nullBal = 0;
    long long subTs = 0;

    MYSQL_BIND r[6] = {};
    r[0].buffer_type = MYSQL_TYPE_STRING; r[0].buffer = dbEmail; r[0].buffer_length = sizeof(dbEmail) - 1;
    r[0].length = &lenEmail; r[0].is_null = &nullEmail;
    r[1].buffer_type = MYSQL_TYPE_STRING; r[1].buffer = dbName; r[1].buffer_length = sizeof(dbName) - 1;
    r[1].length = &lenName; r[1].is_null = &nullName;
    r[2].buffer_type = MYSQL_TYPE_STRING; r[2].buffer = dbAvatar; r[2].buffer_length = sizeof(dbAvatar) - 1;
    r[2].length = &lenAvatar; r[2].is_null = &nullAvatar;
    r[3].buffer_type = MYSQL_TYPE_STRING; r[3].buffer = dbProv; r[3].buffer_length = sizeof(dbProv) - 1;
    r[3].length = &lenProv; r[3].is_null = &nullProv;
    r[4].buffer_type = MYSQL_TYPE_LONGLONG; r[4].buffer = &subTs;
    r[5].buffer_type = MYSQL_TYPE_STRING; r[5].buffer = dbBal; r[5].buffer_length = sizeof(dbBal) - 1;
    r[5].length = &lenBal; r[5].is_null = &nullBal;
    mysql_stmt_bind_result(stmt, r);
    mysql_stmt_store_result(stmt);

    if (mysql_stmt_fetch(stmt) != 0) return std::nullopt;  // 用户不存在

    if (!nullEmail)  dbEmail[lenEmail] = '\0';
    if (!nullName)   dbName[lenName] = '\0';
    if (!nullAvatar) dbAvatar[lenAvatar] = '\0';
    if (!nullProv)   dbProv[lenProv] = '\0';
    if (!nullBal)    dbBal[lenBal] = '\0';

    Profile p;
    p.email        = dbEmail;
    p.username     = dbName;
    p.avatarUrl    = dbAvatar;
    p.provider     = dbProv;
    p.subExpiresTs = subTs;
    p.balance      = dbBal;
    return p;
}

// ═══════════════════════════════════════════════════════
//  管理操作 (2026-09-09 收敛自 SystemRoutes admin 直连 SQL)
// ═══════════════════════════════════════════════════════
bool UserStore::setStatus(const std::string &email, int status) {
    auto guard = m_pool.acquire(5000);
    if (!guard) return false;
    MYSQL* db = guard->get();

    StmtGuard stmt{mysql_stmt_init(db)};
    const char* sql = "UPDATE users SET status=? WHERE LOWER(email)=LOWER(?)";
    if (mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql)) != 0) return false;
    MYSQL_BIND b[2] = {};
    b[0].buffer_type = MYSQL_TYPE_TINY; b[0].buffer = &status;
    b[1].buffer_type = MYSQL_TYPE_STRING;
    b[1].buffer = (void*)email.c_str(); b[1].buffer_length = (unsigned long)email.size();
    mysql_stmt_bind_param(stmt, b);
    return mysql_stmt_execute(stmt) == 0;
}

int UserStore::adjustSubscriptionDays(const std::string &email, int days) {
    auto guard = m_pool.acquire(5000);
    if (!guard) return -1;
    MYSQL* db = guard->get();

    // 两分支保留旧语义: 减天数时仅对已订阅用户生效 (IS NOT NULL)
    const char* sql = days > 0
        ? "UPDATE users SET subscription_expires=DATE_ADD("
          "GREATEST(COALESCE(subscription_expires,NOW()),NOW()), INTERVAL ? DAY)"
          " WHERE LOWER(email)=LOWER(?)"
        : "UPDATE users SET subscription_expires=DATE_SUB("
          "GREATEST(COALESCE(subscription_expires,NOW()),NOW()), INTERVAL ? DAY)"
          " WHERE LOWER(email)=LOWER(?) AND subscription_expires IS NOT NULL";
    StmtGuard stmt{mysql_stmt_init(db)};
    if (mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql)) != 0) return -1;
    int absDays = std::abs(days);
    MYSQL_BIND b[2] = {};
    b[0].buffer_type = MYSQL_TYPE_LONG; b[0].buffer = &absDays;
    b[1].buffer_type = MYSQL_TYPE_STRING;
    b[1].buffer = (void*)email.c_str(); b[1].buffer_length = (unsigned long)email.size();
    mysql_stmt_bind_param(stmt, b);
    if (mysql_stmt_execute(stmt) != 0) return -1;
    return (int)mysql_stmt_affected_rows(stmt);
}

bool UserStore::setSubscriptionExpires(const std::string &email,
                                       const std::string &expires) {
    auto guard = m_pool.acquire(5000);
    if (!guard) return false;
    MYSQL* db = guard->get();

    StmtGuard stmt{mysql_stmt_init(db)};
    const char* sql = (expires == "clear")
        ? "UPDATE users SET subscription_expires=NULL WHERE LOWER(email)=LOWER(?)"
        : "UPDATE users SET subscription_expires=? WHERE LOWER(email)=LOWER(?)";
    if (mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql)) != 0) return false;

    MYSQL_BIND b[2] = {};
    b[0].buffer_type = MYSQL_TYPE_STRING;
    b[0].buffer = (void*)expires.c_str(); b[0].buffer_length = (unsigned long)expires.size();
    b[1].buffer_type = MYSQL_TYPE_STRING;
    b[1].buffer = (void*)email.c_str(); b[1].buffer_length = (unsigned long)email.size();
    mysql_stmt_bind_param(stmt, b);
    return mysql_stmt_execute(stmt) == 0;
}

// ═══════════════════════════════════════════════════════
//  设备风控 (2026-09-09 收敛自 SystemRoutes payload/key)
// ═══════════════════════════════════════════════════════
uint64_t UserStore::deviceBindCount(uint64_t uid) {
    auto guard = m_pool.acquire(5000);
    if (!guard) return 0;
    MYSQL* db = guard->get();

    StmtGuard stmt{mysql_stmt_init(db)};
    const char* sql =
        "SELECT COUNT(*) FROM device_bind WHERE uid=? AND first_seen > NOW() - INTERVAL 7 DAY";
    if (mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql)) != 0) return 0;
    MYSQL_BIND b[1] = {};
    b[0].buffer_type = MYSQL_TYPE_LONGLONG; b[0].buffer = &uid;
    mysql_stmt_bind_param(stmt, b);
    mysql_stmt_execute(stmt);
    mysql_stmt_store_result(stmt);

    long long cnt = 0;
    MYSQL_BIND r[1] = {};
    r[0].buffer_type = MYSQL_TYPE_LONGLONG; r[0].buffer = &cnt;
    mysql_stmt_bind_result(stmt, r);
    if (mysql_stmt_fetch(stmt) != 0) return 0;
    return (uint64_t)cnt;
}

void UserStore::deviceBindTouch(uint64_t uid, const std::string &devId) {
    auto guard = m_pool.acquire(5000);
    if (!guard) return;
    MYSQL* db = guard->get();

    StmtGuard stmt{mysql_stmt_init(db)};
    const char* sql =
        "INSERT INTO device_bind(uid,device_id,first_seen,last_seen) VALUES(?,?,NOW(),NOW()) "
        "ON DUPLICATE KEY UPDATE last_seen=NOW()";
    mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql));
    MYSQL_BIND b[2] = {};
    b[0].buffer_type = MYSQL_TYPE_LONGLONG; b[0].buffer = &uid;
    b[1].buffer_type = MYSQL_TYPE_STRING;
    b[1].buffer = (void*)devId.c_str(); b[1].buffer_length = (unsigned long)devId.size();
    mysql_stmt_bind_param(stmt, b);
    mysql_stmt_execute(stmt);
}

bool UserStore::updateUsername(uint64_t id, const std::string &username) {
    auto guard = m_pool.acquire(5000);
    if (!guard) return false;
    MYSQL* db = guard->get();

    StmtGuard stmt{mysql_stmt_init(db)};
    const char* sql = "UPDATE users SET username=? WHERE id=?";
    if (mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql)) != 0) return false;
    MYSQL_BIND b[2] = {};
    b[0].buffer_type = MYSQL_TYPE_STRING;
    b[0].buffer = (void*)username.c_str(); b[0].buffer_length = (unsigned long)username.size();
    b[1].buffer_type = MYSQL_TYPE_LONGLONG; b[1].buffer = &id;
    mysql_stmt_bind_param(stmt, b);
    return mysql_stmt_execute(stmt) == 0;
}
