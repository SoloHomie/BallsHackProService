cmake_minimum_required(VERSION 3.20)
project(SuretyService
    VERSION 1.0.0
    DESCRIPTION "Surety backend service"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Dependencies ──────────────────────────────────────
# OpenSSL (TLS + crypto: PBKDF2, HMAC, random)
find_package(OpenSSL REQUIRED)

# MySQL client
find_package(unofficial-libmysql CONFIG QUIET)
if(NOT unofficial-libmysql_FOUND)
    # Fallback: try pkg-config / manual paths
    find_path(MYSQL_INCLUDE_DIR NAMES mysql/mysql.h
        HINTS /usr/include/mysql /usr/local/include/mysql)
    find_library(MYSQL_LIBRARY NAMES mysqlclient libmysql
        HINTS /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/local/lib)
    if(MYSQL_INCLUDE_DIR AND MYSQL_LIBRARY)
        add_library(unofficial::libmysql UNKNOWN IMPORTED)
        set_target_properties(unofficial::libmysql PROPERTIES
            IMPORTED_LOCATION "${MYSQL_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${MYSQL_INCLUDE_DIR}")
    else()
        message(FATAL_ERROR "MySQL client library not found. Install: sudo apt install libmysqlclient-dev")
    endif()
endif()

# redis++ (optional — server runs in degraded mode without it)
# First find hiredis (needed as dependency)
find_library(HIREDIS_LIBRARY NAMES hiredis)
find_path(HIREDIS_INCLUDE_DIR NAMES hiredis/hiredis.h)

set(SUREITY_REDIS_FOUND FALSE)

if(HIREDIS_LIBRARY AND HIREDIS_INCLUDE_DIR)
    # Find redis++ manually (skip redis++-config.cmake — it requires
    # hiredis cmake config which apt's libhiredis-dev doesn't ship)
    find_path(REDISPP_INCLUDE_DIR NAMES sw/redis++/redis++.h
        HINTS /usr/local/include)
    find_library(REDISPP_LIBRARY NAMES redis++
        HINTS /usr/local/lib)
    if(REDISPP_INCLUDE_DIR AND REDISPP_LIBRARY)
        message(STATUS "Found redis++: ${REDISPP_LIBRARY}")
        set(SUREITY_REDIS_FOUND TRUE)
    endif()
endif()

if(NOT SUREITY_REDIS_FOUND)
    message(WARNING "redis++ not found — building without Redis support (caching disabled)")
    add_compile_definitions(SUREITY_NO_REDIS)
endif()

# pthread
find_package(Threads REQUIRED)

# curl (OAuth HTTP requests + SMTP email)
find_package(CURL REQUIRED)

# ── Sources ───────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/core/server/HttpServer.cpp
    src/core/server/TlsContext.cpp
    src/core/server/TlsConnection.cpp
    src/auth/AuthService.cpp
    src/auth/CodeManager.cpp
    src/auth/UserStore.cpp
    src/auth/TokenService.cpp
    src/auth/OAuthService.cpp
    src/email/SmtpClient.cpp
    src/email/EmailService.cpp
    src/view/OAuthPage.cpp
    src/view/EmailPage.cpp
    src/core/database/DbMigration.cpp
    src/route/AuthRoutes.cpp
    src/route/WalletRoutes.cpp
    src/route/SystemRoutes.cpp
    src/route/PaymentRoutes.cpp
    src/route/WelfareRoutes.cpp
    src/route/RedeemRoutes.cpp
    src/route/SyncRoutes.cpp
    src/route/AssetRoutes.cpp
    src/route/PeerRoutes.cpp
    src/payment/VPayService.cpp
    src/sync/SyncService.cpp
)

set(HEADERS
    src/core/config/ServerConfig.h
    src/core/config/EmailConfig.h
    src/core/config/OAuthConfig.h
    src/core/config/Env.h
    src/core/config/DbConfig.h
    src/core/server/HttpServer.h
    src/core/server/TlsContext.h
    src/core/server/TlsConnection.h
    src/auth/AuthService.h
    src/auth/CodeManager.h
    src/auth/UserStore.h
    src/auth/TokenService.h
    src/auth/OAuthService.h
    src/auth/PasswordHash.h
    src/auth/SessionManager.h
    src/email/SmtpClient.h
    src/email/EmailService.h
    src/common/StringUtils.h
    src/common/Snowflake.h
    src/common/Logger.h
    src/common/JsonConfig.h
    src/view/OAuthPage.h
    src/view/EmailPage.h
    src/core/cache/RedisClient.h
    src/core/database/DbPool.h
    src/core/database/Schema.h
    src/core/database/DbMigration.h
    src/route/AuthRoutes.h
    src/route/WalletRoutes.h
    src/route/SystemRoutes.h
    src/route/PaymentRoutes.h
    src/route/WelfareRoutes.h
    src/route/RedeemRoutes.h
    src/route/SyncRoutes.h
    src/route/AssetRoutes.h
    src/route/PeerRoutes.h
    src/payment/VPayService.h
    src/sync/SyncService.h
)

# ── Executable ────────────────────────────────────────
add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(${PROJECT_NAME} PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    unofficial::libmysql
    CURL::libcurl
    Threads::Threads)

# redis++: use manually found libs or cmake target
if(SUREITY_REDIS_FOUND)
    if(TARGET redis++_static)
        target_link_libraries(${PROJECT_NAME} PRIVATE redis++_static)
    elseif(TARGET redis++)
        target_link_libraries(${PROJECT_NAME} PRIVATE redis++)
    elseif(REDISPP_LIBRARY)
        target_include_directories(${PROJECT_NAME} PRIVATE ${REDISPP_INCLUDE_DIR})
        target_link_libraries(${PROJECT_NAME} PRIVATE ${REDISPP_LIBRARY} ${HIREDIS_LIBRARY})
    endif()
endif()

# ── Copy html/ to build directory ─────────────────────
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/html"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/html")

# ── Compiler options ──────────────────────────────────
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8 /W3)
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE _popen=popen _pclose=pclose)
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wno-format)
endif()
