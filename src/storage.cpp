#include "jh/mail.hpp"
#include "jh/storage.hpp"

#include "jh/db.hpp"

#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace jh::storage {

namespace {

std::mutex g_mu;
std::string g_data_dir;

std::string save_path(const std::string& acc, int area) {
  std::string safe = acc;
  for (char& c : safe) {
    if (c == '/' || c == '\\' || c == ':') {
      c = '_';
    }
  }
  return g_data_dir + "/saves/" + safe + "_" + std::to_string(area) + ".json";
}

AccountAuthInfo to_auth_info(const DbAccount& db_acc) {
  AccountAuthInfo info;
  info.id = db_acc.id;
  info.acc = db_acc.acc;
  info.psw_salt = db_acc.psw_salt;
  info.psw_hash = db_acc.psw_hash;
  info.created_at = db_acc.created_at;
  info.has_password = db_acc.has_password();
  return info;
}

}  // namespace

void init(const std::string& data_dir) {
  g_data_dir = data_dir;
  fs::create_directories(data_dir + "/saves");
}

AccountRecord ensure_account(const std::string& acc) {
  const DbAccount db_acc = db_ensure_account(acc);
  return {db_acc.id, db_acc.acc};
}

std::optional<AccountRecord> find_account(const std::string& acc) {
  if (auto db_acc = db_find_account(acc)) {
    return AccountRecord{db_acc->id, db_acc->acc};
  }
  return std::nullopt;
}

std::optional<AccountAuthInfo> get_account_auth(const std::string& acc) {
  if (auto db_acc = db_find_account(acc)) {
    return to_auth_info(*db_acc);
  }
  return std::nullopt;
}

std::optional<AccountRecord> create_account_with_password(const std::string& acc, const std::string& salt,
                                                          const std::string& hash) {
  if (auto db_acc = db_create_account(acc, salt, hash)) {
    return AccountRecord{db_acc->id, db_acc->acc};
  }
  return std::nullopt;
}

bool set_account_password(const std::string& acc, const std::string& salt, const std::string& hash) {
  return db_set_password(acc, salt, hash);
}

bool save_cloud(const std::string& acc, int area, const std::string& save_json, const SaveMeta& meta) {
  std::lock_guard<std::mutex> lock(g_mu);
  db_ensure_account(acc);
  std::ofstream out(save_path(acc, area));
  if (!out) {
    return false;
  }
  const int64_t now = std::time(nullptr);
  nlohmann::json wrapper{
      {"save", save_json},
      {"updated_at", now},
      {"username", meta.username},
      {"lev", meta.lev},
      {"save_time", meta.save_time > 0 ? meta.save_time : now},
  };
  out << wrapper.dump();
  return true;
}

std::optional<SaveMeta> get_save_meta(const std::string& acc, int area) {
  std::lock_guard<std::mutex> lock(g_mu);
  std::ifstream in(save_path(acc, area));
  if (!in) {
    return std::nullopt;
  }
  try {
    nlohmann::json j;
    in >> j;
    SaveMeta meta;
    meta.username = j.value("username", acc);
    meta.lev = j.value("lev", 1);
    meta.save_time = j.value("save_time", j.value("updated_at", static_cast<int64_t>(0)));
    return meta;
  } catch (...) {
  }
  return std::nullopt;
}

std::optional<std::string> load_cloud(const std::string& acc, int area) {
  std::lock_guard<std::mutex> lock(g_mu);
  std::ifstream in(save_path(acc, area));
  if (!in) {
    return std::nullopt;
  }
  try {
    nlohmann::json j;
    in >> j;
    if (j.contains("save")) {
      if (j["save"].is_string()) {
        return j["save"].get<std::string>();
      }
      return j["save"].dump();
    }
  } catch (...) {
  }
  return std::nullopt;
}

std::vector<AccountRecord> list_accounts() {
  std::vector<AccountRecord> out;
  for (const auto& db_acc : db_list_accounts()) {
    out.push_back({db_acc.id, db_acc.acc});
  }
  return out;
}

std::vector<AccountAuthInfo> list_accounts_auth() {
  std::vector<AccountAuthInfo> out;
  for (const auto& db_acc : db_list_accounts()) {
    out.push_back(to_auth_info(db_acc));
  }
  return out;
}

std::vector<SaveRecord> list_saves() {
  std::lock_guard<std::mutex> lock(g_mu);
  std::vector<SaveRecord> out;
  const fs::path saves_dir = fs::path(g_data_dir) / "saves";
  if (!fs::exists(saves_dir)) {
    return out;
  }
  for (const auto& entry : fs::directory_iterator(saves_dir)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string filename = entry.path().filename().string();
    const auto us = filename.rfind('_');
    const auto dot = filename.rfind('.');
    if (us == std::string::npos || dot == std::string::npos || us >= dot) {
      continue;
    }
    SaveRecord rec;
    rec.acc = filename.substr(0, us);
    try {
      rec.area = std::stoi(filename.substr(us + 1, dot - us - 1));
    } catch (...) {
      continue;
    }
    rec.size = static_cast<size_t>(entry.file_size());
    rec.updated_at = 0;
    try {
      std::ifstream in(entry.path());
      nlohmann::json j;
      in >> j;
      if (j.contains("updated_at")) {
        rec.updated_at = j["updated_at"].get<int64_t>();
      }
    } catch (...) {
    }
    out.push_back(std::move(rec));
  }
  return out;
}

bool delete_save(const std::string& acc, int area) {
  std::lock_guard<std::mutex> lock(g_mu);
  return fs::remove(save_path(acc, area));
}

bool delete_account(const std::string& acc) {
  if (!db_delete_account(acc)) {
    return false;
  }

  const fs::path saves_dir = fs::path(g_data_dir) / "saves";
  const std::string prefix = save_path(acc, 0);
  const auto pos = prefix.rfind('_');
  const std::string acc_prefix = pos == std::string::npos ? prefix : prefix.substr(0, pos + 1);
  if (fs::exists(saves_dir)) {
    for (const auto& entry : fs::directory_iterator(saves_dir)) {
      const std::string name = entry.path().filename().string();
      if (name.rfind(acc_prefix, 0) == 0) {
        fs::remove(entry.path());
      }
    }
  }
  mail::remove_account(acc);
  return true;
}

ServerStats get_stats() {
  ServerStats stats;
  stats.account_count = db_account_count();
  stats.next_id = db_next_id_hint();
  const fs::path saves_dir = fs::path(g_data_dir) / "saves";
  if (fs::exists(saves_dir)) {
    for (const auto& entry : fs::directory_iterator(saves_dir)) {
      if (entry.is_regular_file()) {
        ++stats.save_count;
      }
    }
  }
  return stats;
}

}  // namespace jh::storage
