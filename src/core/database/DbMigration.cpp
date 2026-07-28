#include "DbMigration.h"
#include "Schema.h"
#include <iostream>
#include <vector>

bool DbMigration::run(DbPool& pool) {
    auto guard = pool.acquire(5000);
    if (!guard) {
        std::cerr << "[DbMigration] cannot acquire DB connection" << std::endl;
        return false;
    }
    MYSQL* db = guard->get();

    std::vector<const char*> tables = {
        Schema::USERS,
        Schema::OAUTH_LINKS,
        Schema::PAYMENT_ORDERS,
        Schema::WELFARE_EVENTS,
        Schema::WELFARE_CLAIMS,
        Schema::ROOMS,
    };

    for (const char* ddl : tables) {
        if (mysql_query(db, ddl) != 0) {
            std::cerr << "[DbMigration] table creation failed: " << mysql_error(db) << std::endl;
            return false;
        }
    }

    std::cout << "[DbMigration] database tables ready" << std::endl;
    return true;
}
