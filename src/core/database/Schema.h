#pragma once

// BallsHackProService — 核心表

namespace Schema {

    constexpr const char* USERS = R"SQL(
CREATE TABLE IF NOT EXISTS users (
    id              BIGINT UNSIGNED PRIMARY KEY,
    email           VARCHAR(255)  NOT NULL,
    username        VARCHAR(255)  NOT NULL DEFAULT '',
    salt            CHAR(64)      NOT NULL,
    password_hash   CHAR(64)      NOT NULL DEFAULT '',
    avatar_url      VARCHAR(512)  DEFAULT NULL,
    provider              VARCHAR(32)  NOT NULL DEFAULT 'local',
    subscription_expires  DATETIME     DEFAULT NULL,
    balance               BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '金额(分)',
    created_at            DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at      DATETIME      NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    UNIQUE KEY uk_email (email)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
)SQL";

    // 验证码由 Redis 管理 (CodeManager)，无需 MySQL 表

    constexpr const char* OAUTH_LINKS = R"SQL(
CREATE TABLE IF NOT EXISTS oauth_links (
    id          BIGINT UNSIGNED PRIMARY KEY,
    user_id     BIGINT UNSIGNED NOT NULL,
    provider    VARCHAR(32)     NOT NULL,
    provider_id VARCHAR(255)    NOT NULL,
    created_at  DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE KEY  uk_oauth (provider, provider_id),
    INDEX       idx_user_id (user_id),
    CONSTRAINT  fk_oauth_user FOREIGN KEY (user_id)
        REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
)SQL";

    constexpr const char* PAYMENT_ORDERS = R"SQL(
CREATE TABLE IF NOT EXISTS payment_orders (
    id              BIGINT UNSIGNED PRIMARY KEY,
    user_id         BIGINT UNSIGNED NOT NULL,
    amount          BIGINT UNSIGNED NOT NULL COMMENT '支付金额(分)',
    duration_days   INT UNSIGNED NOT NULL DEFAULT 30 COMMENT '订阅天数',
    status          VARCHAR(32) NOT NULL DEFAULT 'pending',
    zp_trade_no     VARCHAR(128) DEFAULT '',
    created_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_user_id (user_id),
    INDEX idx_trade_no (zp_trade_no)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
)SQL";

    constexpr const char* WELFARE_EVENTS = R"SQL(
CREATE TABLE IF NOT EXISTS welfare_events (
    id              BIGINT UNSIGNED PRIMARY KEY,
    name            VARCHAR(128) NOT NULL,
    description     VARCHAR(512) NOT NULL DEFAULT '',
    event_type      VARCHAR(32)  NOT NULL DEFAULT 'general' COMMENT 'new_user|holiday|general',
    reward_amount   INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'BallsHackPro 数量',
    starts_at       DATETIME     NOT NULL,
    expires_at      DATETIME     NOT NULL,
    is_active       TINYINT(1)   NOT NULL DEFAULT 1,
    created_at      DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_active_time (is_active, starts_at, expires_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
)SQL";

    constexpr const char* REFRESH_TOKENS = R"SQL(
CREATE TABLE IF NOT EXISTS refresh_tokens (
    id          BIGINT UNSIGNED PRIMARY KEY,
    token_hash  CHAR(64)      NOT NULL,
    user_id     BIGINT UNSIGNED NOT NULL,
    email       VARCHAR(255)  NOT NULL,
    provider    VARCHAR(32)   NOT NULL DEFAULT 'local',
    expires_at  DATETIME      NOT NULL,
    created_at  DATETIME      NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE KEY uk_token_hash (token_hash),
    INDEX idx_user_id (user_id),
    INDEX idx_expires (expires_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
)SQL";

    constexpr const char* ROOMS = R"SQL(
CREATE TABLE IF NOT EXISTS rooms (
    id              BIGINT UNSIGNED PRIMARY KEY,
    room_code       VARCHAR(8)   NOT NULL,
    room_name       VARCHAR(64)  NOT NULL,
    creator_email   VARCHAR(255) NOT NULL,
    controller_email VARCHAR(255) DEFAULT NULL,
    controlled_email VARCHAR(255) DEFAULT NULL,
    status          TINYINT      NOT NULL DEFAULT 0 COMMENT '0=waiting,1=ready,2=closed',
    created_at      DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at      DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    UNIQUE KEY uk_room_code (room_code)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
)SQL";

    constexpr const char* WELFARE_CLAIMS = R"SQL(
CREATE TABLE IF NOT EXISTS welfare_claims (
    id              BIGINT UNSIGNED PRIMARY KEY,
    user_id         BIGINT UNSIGNED NOT NULL,
    event_id        BIGINT UNSIGNED NOT NULL,
    claimed_at      DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE KEY uk_user_event (user_id, event_id),
    INDEX idx_user_id (user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
)SQL";

}
