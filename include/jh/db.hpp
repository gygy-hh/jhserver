#pragma once

#include "jh/config.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace jh {

struct DbAccount {
  uint32_t id = 0;
  std::string acc;
  std::string psw_salt;
  std::string psw_hash;
  int64_t created_at = 0;

  bool has_password() const { return !psw_hash.empty(); }
};

struct DbSession {
  std::string acc;
  int64_t expires_at = 0;
};

bool db_init(const MysqlConfig& config, const std::string& admin_acc, const std::string& admin_psw,
             const std::string& legacy_accounts_path = "");
void db_shutdown();

std::optional<DbAccount> db_find_account(const std::string& acc);
std::optional<DbAccount> db_create_account(const std::string& acc, const std::string& salt, const std::string& hash);
DbAccount db_ensure_account(const std::string& acc);
bool db_set_password(const std::string& acc, const std::string& salt, const std::string& hash);
bool db_delete_account(const std::string& acc);
std::vector<DbAccount> db_list_accounts();
size_t db_account_count();
uint32_t db_next_id_hint();

bool db_insert_session(const std::string& acc, const std::string& token_hash, int64_t expires_at);
std::optional<DbSession> db_find_session(const std::string& token_hash);
bool db_revoke_sessions(const std::string& acc);

}  // namespace jh
