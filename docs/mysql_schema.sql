CREATE DATABASE IF NOT EXISTS cachelite
    CHARACTER SET utf8mb4
    COLLATE utf8mb4_bin;

USE cachelite;

CREATE TABLE IF NOT EXISTS cache_entries (
    cache_key VARCHAR(255) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin
        NOT NULL,
    cache_value LONGTEXT NOT NULL,
    PRIMARY KEY (cache_key)
) ENGINE = InnoDB;

INSERT INTO cache_entries (cache_key, cache_value)
VALUES ('demo:key', 'value from mysql')
ON DUPLICATE KEY UPDATE cache_value = VALUES(cache_value);
