#include "jh/db.hpp"

#include "jh/crypto.hpp"

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

std::mutex g_mu;
MYSQL* g_conn = nullptr;
MysqlConfig g_cfg;

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
  if (!g_conn) {
    throw std::runtime_error("mysql not connected");
  }
  std::string out;
  out.resize(input.size() * 2 + 1);
  const unsigned long len = mysql_real_escape_string(g_conn, out.data(), input.c_str(),
                                                     static_cast<unsigned long>(input.size()));
  out.resize(len);
  return out;
}

void exec_sql(const std::string& sql) {
  if (!g_conn) {
    throw std::runtime_error("mysql not connected");
  }
  if (mysql_query(g_conn, sql.c_str()) != 0) {
    throw std::runtime_error(std::string("mysql query failed: ") + mysql_error(g_conn) + " SQL: " + sql);
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
  return acc;
}

std::optional<DbAccount> query_one_account_unlocked(const std::string& sql) {
  exec_sql(sql);
  MYSQL_RES* res = mysql_store_result(g_conn);
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
  return query_one_account_unlocked("SELECT id, acc, psw_salt, psw_hash, created_at FROM accounts WHERE acc='" +
                                    sql_escape(acc) + "' LIMIT 1");
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
      PRIMARY KEY (id),
      UNIQUE KEY uk_acc (acc)
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
  exec_sql("INSERT INTO accounts (acc, psw_salt, psw_hash, created_at) VALUES ('" + sql_escape(admin_acc) + "','" +
           sql_escape(salt) + "','" + sql_escape(hash) + "'," + std::to_string(ts) +
           ") ON DUPLICATE KEY UPDATE psw_salt=VALUES(psw_salt), psw_hash=VALUES(psw_hash)");
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
  std::lock_guard<std::mutex> lock(g_mu);
  if (!g_conn) {
    throw std::runtime_error("mysql not connected");
  }
  if (auto found = find_account_unlocked(acc)) {
    return *found;
  }
  const int64_t ts = now_sec();
  exec_sql("INSERT INTO accounts (acc, psw_salt, psw_hash, created_at) VALUES ('" + sql_escape(acc) +
           "','',''," + std::to_string(ts) + ")");
  if (auto created = find_account_unlocked(acc)) {
    return *created;
  }
  throw std::runtime_error("failed to create account");
}

bool db_init(const MysqlConfig& config, const std::string& admin_acc, const std::string& admin_psw,
             const std::string& legacy_accounts_path) {
  std::lock_guard<std::mutex> lock(g_mu);
  g_cfg = config;

  if (g_conn) {
    mysql_close(g_conn);
    g_conn = nullptr;
  }

  g_conn = mysql_init(nullptr);
  if (!g_conn) {
    std::cerr << "[db] mysql_init failed\n";
    return false;
  }

  my_bool reconnect = 1;
  mysql_options(g_conn, MYSQL_OPT_RECONNECT, &reconnect);
  unsigned int timeout_sec = 10;
  mysql_options(g_conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout_sec);
#ifdef MYSQL_OPT_SSL_ENFORCE
  {
    my_bool ssl_enforce = 0;
    mysql_options(g_conn, MYSQL_OPT_SSL_ENFORCE, &ssl_enforce);
  }
#endif
#ifdef MYSQL_OPT_SSL_MODE
  {
    const int ssl_mode = 1;  // SSL_MODE_DISABLED
    mysql_options(g_conn, MYSQL_OPT_SSL_MODE, &ssl_mode);
  }
#endif
#ifdef MYSQL_PLUGIN_DIR
  mysql_options(g_conn, MYSQL_PLUGIN_DIR, "");
#endif
#ifdef MYSQL_DEFAULT_AUTH
  mysql_options(g_conn, MYSQL_DEFAULT_AUTH, "mysql_native_password");
#endif

  if (!mysql_real_connect(g_conn, config.host.c_str(), config.user.c_str(), config.password.c_str(), nullptr,
                          static_cast<unsigned int>(config.port), nullptr, 0)) {
    std::cerr << "[db] connect failed: " << mysql_error(g_conn) << "\n";
    mysql_close(g_conn);
    g_conn = nullptr;
    return false;
  }

  try {
    ensure_schema();
    import_legacy_accounts(legacy_accounts_path);
    upsert_admin(admin_acc, admin_psw);
  } catch (const std::exception& ex) {
    std::cerr << "[db] init failed: " << ex.what() << "\n";
    mysql_close(g_conn);
    g_conn = nullptr;
    return false;
  }

  std::cout << "[db] connected to " << config.host << ":" << config.port << "/" << config.database << "\n";
  return true;
}

void db_shutdown() {
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_conn) {
    mysql_close(g_conn);
    g_conn = nullptr;
  }
}

std::optional<DbAccount> db_find_account(const std::string& acc) {
  std::lock_guard<std::mutex> lock(g_mu);
  if (!g_conn) {
    return std::nullopt;
  }
  return find_account_unlocked(acc);
}

std::optional<DbAccount> db_create_account(const std::string& acc, const std::string& salt, const std::string& hash) {
  std::lock_guard<std::mutex> lock(g_mu);
  if (!g_conn) {
    return std::nullopt;
  }
  if (find_account_unlocked(acc)) {
    return std::nullopt;
  }
  const int64_t ts = now_sec();
  exec_sql("INSERT INTO accounts (acc, psw_salt, psw_hash, created_at) VALUES ('" + sql_escape(acc) + "','" +
           sql_escape(salt) + "','" + sql_escape(hash) + "'," + std::to_string(ts) + ")");
  return find_account_unlocked(acc);
}

bool db_set_password(const std::string& acc, const std::string& salt, const std::string& hash) {
  std::lock_guard<std::mutex> lock(g_mu);
  if (!g_conn) {
    return false;
  }
  const std::string sql = "UPDATE accounts SET psw_salt='" + sql_escape(salt) + "', psw_hash='" + sql_escape(hash) +
                          "' WHERE acc='" + sql_escape(acc) + "'";
  exec_sql(sql);
  return mysql_affected_rows(g_conn) > 0;
}

bool db_delete_account(const std::string& acc) {
  std::lock_guard<std::mutex> lock(g_mu);
  if (!g_conn) {
    return false;
  }
  exec_sql("DELETE FROM accounts WHERE acc='" + sql_escape(acc) + "'");
  return mysql_affected_rows(g_conn) > 0;
}

std::vector<DbAccount> db_list_accounts() {
  std::lock_guard<std::mutex> lock(g_mu);
  std::vector<DbAccount> out;
  if (!g_conn) {
    return out;
  }
  exec_sql("SELECT id, acc, psw_salt, psw_hash, created_at FROM accounts ORDER BY id");
  MYSQL_RES* res = mysql_store_result(g_conn);
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
  std::lock_guard<std::mutex> lock(g_mu);
  if (!g_conn) {
    return 0;
  }
  exec_sql("SELECT COUNT(*) FROM accounts");
  MYSQL_RES* res = mysql_store_result(g_conn);
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
  std::lock_guard<std::mutex> lock(g_mu);
  if (!g_conn) {
    return 1;
  }
  exec_sql("SELECT IFNULL(MAX(id), 0) + 1 FROM accounts");
  MYSQL_RES* res = mysql_store_result(g_conn);
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

}  // namespace jh
