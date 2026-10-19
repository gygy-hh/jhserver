#include "jh/db.hpp"

#include "jh/crypto.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <mysql.h>
#include <nlohmann/json.hpp>
#include <random>
#include <stdexcept>

namespace jh {

namespace {

constexpr size_t kDbPoolSize = 2;
constexpr unsigned int kDupEntry = 1062;

struct DbSlot {
  std::mutex mu;
  MYSQL* conn = nullptr;
};

DbSlot g_slots[kDbPoolSize];
std::atomic<size_t> g_slot_rr{0};
MysqlConfig g_cfg;
thread_local MYSQL* t_dbc = nullptr;

MYSQL* dbc() {
  if (!t_dbc) {
    throw std::runtime_error("mysql not connected");
  }
  return t_dbc;
}

MYSQL* open_mysql(bool select_database) {
  MYSQL* conn = mysql_init(nullptr);
  if (!conn) {
    std::cerr << "[db] mysql_init failed\n";
    return nullptr;
  }
  my_bool reconnect = 1;
  mysql_options(conn, MYSQL_OPT_RECONNECT, &reconnect);
  unsigned int timeout_sec = 10;
  mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout_sec);
#ifdef MYSQL_OPT_SSL_ENFORCE
  {
    my_bool ssl_enforce = 0;
    mysql_options(conn, MYSQL_OPT_SSL_ENFORCE, &ssl_enforce);
  }
#endif
#ifdef MYSQL_OPT_SSL_MODE
  {
    const int ssl_mode = 1;  // SSL_MODE_DISABLED
    mysql_options(conn, MYSQL_OPT_SSL_MODE, &ssl_mode);
  }
#endif
#ifdef MYSQL_PLUGIN_DIR
  mysql_options(conn, MYSQL_PLUGIN_DIR, "");
#endif
#ifdef MYSQL_DEFAULT_AUTH
  mysql_options(conn, MYSQL_DEFAULT_AUTH, "mysql_native_password");
#endif
  const char* database = select_database ? g_cfg.database.c_str() : nullptr;
  if (!mysql_real_connect(conn, g_cfg.host.c_str(), g_cfg.user.c_str(), g_cfg.password.c_str(), database,
                          static_cast<unsigned int>(g_cfg.port), nullptr, 0)) {
    std::cerr << "[db] connect failed: " << mysql_error(conn) << "\n";
    mysql_close(conn);
    return nullptr;
  }
  mysql_set_character_set(conn, "utf8mb4");
  return conn;
}

void close_pool() {
  for (DbSlot& slot : g_slots) {
    std::lock_guard<std::mutex> lock(slot.mu);
    if (slot.conn) {
      mysql_close(slot.conn);
      slot.conn = nullptr;
    }
  }
}

struct DbSlotGuard {
  std::unique_lock<std::mutex> lock;
  MYSQL* previous = nullptr;
  bool nested = false;

  DbSlotGuard() : previous(t_dbc), nested(t_dbc != nullptr) {
    if (nested) {
      return;
    }
    const size_t start = g_slot_rr.fetch_add(1, std::memory_order_relaxed);
    for (size_t n = 0; n < kDbPoolSize; ++n) {
      DbSlot& slot = g_slots[(start + n) % kDbPoolSize];
      std::unique_lock<std::mutex> candidate(slot.mu, std::try_to_lock);
      if (!candidate.owns_lock()) {
        continue;
      }
      lock = std::move(candidate);
      t_dbc = slot.conn;
      return;
    }
    DbSlot& slot = g_slots[start % kDbPoolSize];
    lock = std::unique_lock<std::mutex>(slot.mu);
    t_dbc = slot.conn;
  }

  ~DbSlotGuard() {
    if (!nested) {
      t_dbc = previous;
    }
  }

  explicit operator bool() const { return t_dbc != nullptr; }
};

std::string random_salt() {
  static thread_local std::mt19937 rng(static_cast<unsigned>(
      std::chrono::steady_clock::now().time_since_epoch().count()));
  static const char kHex[] = "0123456789abcdef";
  std::string out(16, '\0');
  for (char& c : out) {
    c = kHex[rng() % 16];
  }
  return out;
}

std::string hash_password(const std::string& salt, const std::string& psw) {
  return crypto::md5_hex(salt + ":" + psw);
}

int64_t now_sec() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string sql_escape(const std::string& input) {
  MYSQL* conn = dbc();
  std::string out;
  out.resize(input.size() * 2 + 1);
  const unsigned long len = mysql_real_escape_string(conn, out.data(), input.c_str(),
                                                     static_cast<unsigned long>(input.size()));
  out.resize(len);
  return out;
}

void exec_sql(const std::string& sql) {
  MYSQL* conn = dbc();
  if (mysql_query(conn, sql.c_str()) != 0) {
    throw std::runtime_error(std::string("mysql query failed: ") + mysql_error(conn) + " SQL: " + sql);
  }
}

DbAccount row_to_account(MYSQL_ROW row) {
  DbAccount acc;
  if (!row[0] || !row[1]) {
    return acc;
  }
  acc.id = static_cast<uint32_t>(std::strtoul(row[0], nullptr, 10));
  acc.acc = row[1];
  acc.psw_salt = row[2] ? row[2] : "";
  acc.psw_hash = row[3] ? row[3] : "";
  acc.created_at = row[4] ? std::strtoll(row[4], nullptr, 10) : 0;
  acc.bl_exempt = row[5] && std::strtol(row[5], nullptr, 10) != 0;
  return acc;
}

std::optional<DbAccount> query_one_account_unlocked(const std::string& sql) {
  exec_sql(sql);
  MYSQL_RES* res = mysql_store_result(dbc());
  if (!res) {
    return std::nullopt;
  }
  MYSQL_ROW row = mysql_fetch_row(res);
  DbAccount acc;
  if (row) {
    acc = row_to_account(row);
  }
  mysql_free_result(res);
  if (acc.id == 0) {
    return std::nullopt;
  }
  return acc;
}

std::optional<DbAccount> find_account_unlocked(const std::string& acc) {
  return query_one_account_unlocked(
      "SELECT id, acc, psw_salt, psw_hash, created_at, bl_exempt FROM accounts WHERE acc='" + sql_escape(acc) +
      "' LIMIT 1");
}

void ensure_bl_exempt_column() {
  exec_sql("SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA='" + sql_escape(g_cfg.database) +
           "' AND TABLE_NAME='accounts' AND COLUMN_NAME='bl_exempt'");
  MYSQL_RES* res = mysql_store_result(dbc());
  if (!res) {
    throw std::runtime_error("mysql store result failed");
  }
  MYSQL_ROW row = mysql_fetch_row(res);
  const bool exists = row && row[0] && std::strtol(row[0], nullptr, 10) > 0;
  mysql_free_result(res);
  if (!exists) {
    exec_sql("ALTER TABLE accounts ADD COLUMN bl_exempt TINYINT NOT NULL DEFAULT 0");
    std::cout << "[db] added accounts.bl_exempt\n";
  }
}

void ensure_schema() {
  exec_sql("CREATE DATABASE IF NOT EXISTS `" + g_cfg.database + "` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci");
  exec_sql("USE `" + g_cfg.database + "`");
  exec_sql(R"(
    CREATE TABLE IF NOT EXISTS accounts (
      id INT UNSIGNED NOT NULL AUTO_INCREMENT,
      acc VARCHAR(128) NOT NULL,
      psw_salt VARCHAR(32) NOT NULL DEFAULT '',
      psw_hash VARCHAR(64) NOT NULL DEFAULT '',
      created_at BIGINT NOT NULL DEFAULT 0,
      bl_exempt TINYINT NOT NULL DEFAULT 0,
      PRIMARY KEY (id),
      UNIQUE KEY uk_acc (acc)
    ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
  )");
  exec_sql(R"(
    CREATE TABLE IF NOT EXISTS sessions (
      token_hash CHAR(32) NOT NULL,
      acc VARCHAR(128) NOT NULL,
      expires_at BIGINT NOT NULL,
      created_at BIGINT NOT NULL,
      PRIMARY KEY (token_hash),
      KEY idx_sessions_acc (acc),
      KEY idx_sessions_expires (expires_at)
    ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
  )");
}

void upsert_admin(const std::string& admin_acc, const std::string& admin_psw) {
  if (admin_acc.empty() || admin_psw.empty()) {
    return;
  }
  const std::string salt = random_salt();
  const std::string hash = hash_password(salt, admin_psw);
  const int64_t ts = now_sec();
  exec_sql("INSERT IGNORE INTO accounts (acc, psw_salt, psw_hash, created_at) VALUES ('" + sql_escape(admin_acc) + "','" +
           sql_escape(salt) + "','" + sql_escape(hash) + "'," + std::to_string(ts) +
           ")");
  std::cout << "[db] admin account ready: " << admin_acc << "\n";
}

void import_legacy_accounts(const std::string& legacy_path) {
  if (legacy_path.empty() || !std::ifstream(legacy_path).good()) {
    return;
  }
  try {
    std::ifstream in(legacy_path);
    nlohmann::json j;
    in >> j;
    if (!j.contains("accounts") || !j["accounts"].is_object()) {
      return;
    }
    int imported = 0;
    for (auto it = j["accounts"].begin(); it != j["accounts"].end(); ++it) {
      const std::string acc = it.key();
      if (find_account_unlocked(acc)) {
        continue;
      }
      const auto& entry = it.value();
      const std::string salt = entry.value("psw_salt", "");
      const std::string hash = entry.value("psw_hash", "");
      const int64_t ts = entry.value("created_at", now_sec());
      exec_sql("INSERT INTO accounts (acc, psw_salt, psw_hash, created_at) VALUES ('" + sql_escape(acc) + "','" +
               sql_escape(salt) + "','" + sql_escape(hash) + "'," + std::to_string(ts) + ")");
      ++imported;
    }
    if (imported > 0) {
      std::cout << "[db] imported " << imported << " accounts from " << legacy_path << "\n";
    }
  } catch (const std::exception& ex) {
    std::cerr << "[db] legacy import failed: " << ex.what() << "\n";
  }
}

}  // namespace

DbAccount db_ensure_account(const std::string& acc) {
  DbSlotGuard guard;
  if (!guard) {
    throw std::runtime_error("mysql not connected");
  }
  if (auto found = find_account_unlocked(acc)) {
    return *found;
  }
  const int64_t ts = now_sec();
  const std::string sql = "INSERT INTO accounts (acc, psw_salt, psw_hash, created_at) VALUES ('" + sql_escape(acc) +
                          "','',''," + std::to_string(ts) + ")";
  if (mysql_query(dbc(), sql.c_str()) != 0) {
    if (mysql_errno(dbc()) == kDupEntry) {
      if (auto found = find_account_unlocked(acc)) {
        return *found;
      }
    }
    throw std::runtime_error(std::string("mysql query failed: ") + mysql_error(dbc()) + " SQL: " + sql);
  }
  if (auto created = find_account_unlocked(acc)) {
    return *created;
  }
  throw std::runtime_error("failed to create account");
}

bool db_init(const MysqlConfig& config, const std::string& admin_acc, const std::string& admin_psw,
             const std::string& legacy_accounts_path) {
  g_cfg = config;
  mysql_library_init(0, nullptr, nullptr);
  close_pool();

  g_slots[0].conn = open_mysql(false);
  if (!g_slots[0].conn) {
    return false;
  }
  t_dbc = g_slots[0].conn;
  try {
    ensure_schema();
    ensure_bl_exempt_column();
    import_legacy_accounts(legacy_accounts_path);
    upsert_admin(admin_acc, admin_psw);
  } catch (const std::exception& ex) {
    std::cerr << "[db] init failed: " << ex.what() << "\n";
    t_dbc = nullptr;
    close_pool();
    return false;
  }
  t_dbc = nullptr;
  mysql_close(g_slots[0].conn);
  g_slots[0].conn = open_mysql(true);
  if (!g_slots[0].conn) {
    close_pool();
    return false;
  }

  for (size_t i = 1; i < kDbPoolSize; ++i) {
    g_slots[i].conn = open_mysql(true);
    if (!g_slots[i].conn) {
      close_pool();
      return false;
    }
  }

  std::cout << "[db] connected to " << config.host << ":" << config.port << "/" << config.database
            << " pool=" << kDbPoolSize << "\n";
  return true;
}

void db_shutdown() {
  t_dbc = nullptr;
  close_pool();
}

std::optional<DbAccount> db_find_account(const std::string& acc) {
  DbSlotGuard guard;
  if (!guard) {
    return std::nullopt;
  }
  return find_account_unlocked(acc);
}

std::optional<DbAccount> db_create_account(const std::string& acc, const std::string& salt, const std::string& hash) {
  DbSlotGuard guard;
  if (!guard) {
    return std::nullopt;
  }
  if (find_account_unlocked(acc)) {
    return std::nullopt;
  }
  const int64_t ts = now_sec();
  const std::string sql = "INSERT INTO accounts (acc, psw_salt, psw_hash, created_at) VALUES ('" + sql_escape(acc) +
                          "','" + sql_escape(salt) + "','" + sql_escape(hash) + "'," + std::to_string(ts) + ")";
  if (mysql_query(dbc(), sql.c_str()) != 0) {
    if (mysql_errno(dbc()) == kDupEntry) {
      return std::nullopt;
    }
    throw std::runtime_error(std::string("mysql query failed: ") + mysql_error(dbc()) + " SQL: " + sql);
  }
  return find_account_unlocked(acc);
}

bool db_set_password(const std::string& acc, const std::string& salt, const std::string& hash) {
  DbSlotGuard guard;
  if (!guard) {
    return false;
  }
  const std::string sql = "UPDATE accounts SET psw_salt='" + sql_escape(salt) + "', psw_hash='" + sql_escape(hash) +
                          "' WHERE acc='" + sql_escape(acc) + "'";
  exec_sql(sql);
  return mysql_affected_rows(dbc()) > 0;
}

bool db_set_bl_exempt(const std::string& acc, bool exempt) {
  DbSlotGuard guard;
  if (!guard || acc.empty()) {
    return false;
  }
  if (!find_account_unlocked(acc)) {
    return false;
  }
  exec_sql("UPDATE accounts SET bl_exempt=" + std::string(exempt ? "1" : "0") + " WHERE acc='" + sql_escape(acc) +
           "'");
  return true;
}

bool db_delete_account(const std::string& acc) {
  DbSlotGuard guard;
  if (!guard) {
    return false;
  }
  exec_sql("DELETE FROM sessions WHERE acc='" + sql_escape(acc) + "'");
  exec_sql("DELETE FROM accounts WHERE acc='" + sql_escape(acc) + "'");
  return mysql_affected_rows(dbc()) > 0;
}

std::vector<DbAccount> db_list_accounts() {
  DbSlotGuard guard;
  std::vector<DbAccount> out;
  if (!guard) {
    return out;
  }
  exec_sql("SELECT id, acc, psw_salt, psw_hash, created_at, bl_exempt FROM accounts ORDER BY id");
  MYSQL_RES* res = mysql_store_result(dbc());
  if (!res) {
    return out;
  }
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res)) != nullptr) {
    out.push_back(row_to_account(row));
  }
  mysql_free_result(res);
  return out;
}

size_t db_account_count() {
  DbSlotGuard guard;
  if (!guard) {
    return 0;
  }
  exec_sql("SELECT COUNT(*) FROM accounts");
  MYSQL_RES* res = mysql_store_result(dbc());
  if (!res) {
    return 0;
  }
  MYSQL_ROW row = mysql_fetch_row(res);
  size_t count = 0;
  if (row && row[0]) {
    count = static_cast<size_t>(std::strtoull(row[0], nullptr, 10));
  }
  mysql_free_result(res);
  return count;
}

uint32_t db_next_id_hint() {
  DbSlotGuard guard;
  if (!guard) {
    return 1;
  }
  exec_sql("SELECT IFNULL(MAX(id), 0) + 1 FROM accounts");
  MYSQL_RES* res = mysql_store_result(dbc());
  if (!res) {
    return 1;
  }
  MYSQL_ROW row = mysql_fetch_row(res);
  uint32_t next = 1;
  if (row && row[0]) {
    next = static_cast<uint32_t>(std::strtoul(row[0], nullptr, 10));
  }
  mysql_free_result(res);
  return next;
}

bool db_insert_session(const std::string& acc, const std::string& token_hash, int64_t expires_at) {
  DbSlotGuard guard;
  if (!guard || acc.empty() || token_hash.size() != 32) {
    return false;
  }
  const int64_t now = now_sec();
  exec_sql("DELETE FROM sessions WHERE expires_at<" + std::to_string(now));
  exec_sql("INSERT INTO sessions (token_hash, acc, expires_at, created_at) VALUES ('" +
           sql_escape(token_hash) + "','" + sql_escape(acc) + "'," + std::to_string(expires_at) + "," +
           std::to_string(now) + ")");
  return true;
}

std::optional<DbSession> db_find_session(const std::string& token_hash) {
  DbSlotGuard guard;
  if (!guard || token_hash.size() != 32) {
    return std::nullopt;
  }
  exec_sql("SELECT acc, expires_at FROM sessions WHERE token_hash='" + sql_escape(token_hash) +
           "' AND expires_at>=" + std::to_string(now_sec()) + " LIMIT 1");
  MYSQL_RES* res = mysql_store_result(dbc());
  if (!res) {
    return std::nullopt;
  }
  MYSQL_ROW row = mysql_fetch_row(res);
  std::optional<DbSession> out;
  if (row && row[0] && row[1]) {
    out = DbSession{row[0], std::strtoll(row[1], nullptr, 10)};
  }
  mysql_free_result(res);
  return out;
}

bool db_revoke_sessions(const std::string& acc) {
  DbSlotGuard guard;
  if (!guard || acc.empty()) {
    return false;
  }
  exec_sql("DELETE FROM sessions WHERE acc='" + sql_escape(acc) + "'");
  return true;
}

void db_exec(const std::string& sql) {
  DbSlotGuard guard;
  exec_sql(sql);
  MYSQL_RES* res = mysql_store_result(dbc());
  if (res) {
    mysql_free_result(res);
  }
}

std::string db_escape(const std::string& input) {
  DbSlotGuard guard;
  return sql_escape(input);
}

std::vector<std::vector<std::string>> db_query(const std::string& sql) {
  DbSlotGuard guard;
  exec_sql(sql);
  MYSQL_RES* res = mysql_store_result(dbc());
  std::vector<std::vector<std::string>> rows;
  if (!res) {
    return rows;
  }
  const unsigned int columns = mysql_num_fields(res);
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res)) != nullptr) {
    unsigned long* lengths = mysql_fetch_lengths(res);
    std::vector<std::string> cells(columns);
    for (unsigned int i = 0; i < columns; ++i) {
      if (row[i] && lengths) {
        cells[i].assign(row[i], lengths[i]);
      }
    }
    rows.push_back(std::move(cells));
  }
  mysql_free_result(res);
  return rows;
}

}  // namespace jh
