CREATE DATABASE IF NOT EXISTS jh_game CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
USE jh_game;

CREATE TABLE IF NOT EXISTS accounts (
  id INT UNSIGNED NOT NULL AUTO_INCREMENT,
  acc VARCHAR(128) NOT NULL,
  psw_salt VARCHAR(32) NOT NULL DEFAULT '',
  psw_hash VARCHAR(64) NOT NULL DEFAULT '',
  created_at BIGINT NOT NULL DEFAULT 0,
  PRIMARY KEY (id),
  UNIQUE KEY uk_acc (acc)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS sessions (
  token_hash CHAR(32) NOT NULL,
  acc VARCHAR(128) NOT NULL,
  expires_at BIGINT NOT NULL,
  created_at BIGINT NOT NULL,
  PRIMARY KEY (token_hash),
  KEY idx_sessions_acc (acc),
  KEY idx_sessions_expires (expires_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
