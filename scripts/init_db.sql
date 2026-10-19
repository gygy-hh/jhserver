CREATE DATABASE IF NOT EXISTS jh_game CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
USE jh_game;

CREATE TABLE IF NOT EXISTS accounts (
  id INT UNSIGNED NOT NULL AUTO_INCREMENT,
  acc VARCHAR(128) NOT NULL,
  psw_salt VARCHAR(32) NOT NULL DEFAULT '',
  psw_hash VARCHAR(64) NOT NULL DEFAULT '',
  created_at BIGINT NOT NULL DEFAULT 0,
  bl_exempt TINYINT NOT NULL DEFAULT 0,
  PRIMARY KEY (id),
  UNIQUE KEY uk_acc (acc)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

ALTER TABLE accounts ADD COLUMN IF NOT EXISTS bl_exempt TINYINT NOT NULL DEFAULT 0;

CREATE TABLE IF NOT EXISTS lunjian_fighter (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  channel VARCHAR(64) NOT NULL,
  account VARCHAR(128) NOT NULL,
  area INT NOT NULL,
  week_id BIGINT NOT NULL,
  nick VARCHAR(64) NOT NULL DEFAULT '',
  head INT NOT NULL DEFAULT 0,
  fight_power INT NOT NULL DEFAULT 0,
  score INT NOT NULL DEFAULT 0,
  times_left INT NOT NULL DEFAULT 25,
  times_day BIGINT NOT NULL DEFAULT 0,
  fight_data MEDIUMTEXT,
  loadout_json TEXT,
  is_npc TINYINT NOT NULL DEFAULT 0,
  utime BIGINT NOT NULL DEFAULT 0,
  PRIMARY KEY (id),
  UNIQUE KEY uk_lj (channel, account, area, week_id),
  KEY idx_lj_match (channel, week_id, score)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

ALTER TABLE lunjian_fighter
  CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS sessions (
  token_hash CHAR(32) NOT NULL,
  acc VARCHAR(128) NOT NULL,
  expires_at BIGINT NOT NULL,
  created_at BIGINT NOT NULL,
  PRIMARY KEY (token_hash),
  KEY idx_sessions_acc (acc),
  KEY idx_sessions_expires (expires_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
