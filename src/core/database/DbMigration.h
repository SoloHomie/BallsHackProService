#pragma once
#include "DbPool.h"

// ── 数据库迁移（幂等，每次启动安全执行）───────────────────
class DbMigration {
public:
    static bool run(DbPool& pool);
};
