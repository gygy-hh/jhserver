#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace jh::storage {

struct AccountRecord {
  uint32_t id = 0;
  std::string acc;
};

struct AccountAuthInfo {
  uint32_t id = 0;
  std::string acc;
  bool has_password = false;
  std::string psw_salt;
  std::string psw_hash;
  int64_t created_at = 0;
};

struct SaveRecord {
  std::string acc;
  int area = 0;
  int64_t updated_at = 0;
  size_t size = 0;
};

struct SaveMeta {
  std::string username;
  int lev = 1;
  int64_t save_time = 0;
};

struct ServerStats {
  size_t account_count = 0;
  size_t save_count = 0;
  uint32_t next_id = 1;
};

void init(const std::string& data_dir);

AccountRecord ensure_account(const std::string& acc);
std::optional<AccountRecord> find_account(const std::string& acc);
std::optional<AccountAuthInfo> get_account_auth(const std::string& acc);
std::optional<AccountRecord> create_account_with_password(const std::string& acc, const std::string& salt,
                                                          const std::string& hash);
bool set_account_password(const std::string& acc, const std::string& salt, const std::string& hash);
std::vector<AccountRecord> list_accounts();
std::vector<AccountAuthInfo> list_accounts_auth();

bool save_cloud(const std::string& acc, int area, const std::string& save_json, const SaveMeta& meta);
std::optional<std::string> load_cloud(const std::string& acc, int area);
std::optional<SaveMeta> get_save_meta(const std::string& acc, int area);
std::vector<SaveRecord> list_saves();
bool delete_save(const std::string& acc, int area);
bool delete_account(const std::string& acc);

ServerStats get_stats();

}  // namespace jh::storage
