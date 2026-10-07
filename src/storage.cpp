#include "jh/storage.hpp"

#include "jh/cache.hpp"
#include "jh/db.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace jh::storage {

namespace {

std::mutex g_mu;
std::string g_data_dir;
TtlCache<std::string, AccountAuthInfo> g_account_cache(100000);
constexpr auto kAccountCacheTtl = std::chrono::minutes(10);

std::string save_path(const std::string& acc, int area) {
  std::string safe = acc;
  for (char& c : safe) {
    if (c == '/' || c == '\\' || c == ':') {
      c = '_';
    }
  }
  return g_data_dir + "/saves/" + safe + "_" + std::to_string(area) + ".json";
}

std::string save_blob_path(const std::string& acc, int area) {
  std::string p = save_path(acc, area);
  if (p.size() > 5 && p.substr(p.size() - 5) == ".json") {
    p.replace(p.size() - 5, 5, ".sav");
  } else {
    p += ".sav";
  }
  return p;
}

AccountAuthInfo to_auth_info(const DbAccount& db_acc) {
  AccountAuthInfo info;
  info.id = db_acc.id;
  info.acc = db_acc.acc;
  info.psw_salt = db_acc.psw_salt;
  info.psw_hash = db_acc.psw_hash;
  info.created_at = db_acc.created_at;
  info.has_password = db_acc.has_password();
  info.bl_exempt = db_acc.bl_exempt;
  return info;
}

}  // namespace

void init(const std::string& data_dir) {
  g_data_dir = data_dir;
  g_account_cache.clear();
  fs::create_directories(data_dir + "/saves");
}

AccountRecord ensure_account(const std::string& acc) {
  if (const auto cached = g_account_cache.get(acc)) {
    return {cached->id, cached->acc};
  }
  const DbAccount db_acc = db_ensure_account(acc);
  g_account_cache.put(acc, to_auth_info(db_acc), kAccountCacheTtl);
  return {db_acc.id, db_acc.acc};
}

std::optional<AccountRecord> find_account(const std::string& acc) {
  if (const auto cached = g_account_cache.get(acc)) {
    return AccountRecord{cached->id, cached->acc};
  }
  if (auto db_acc = db_find_account(acc)) {
    g_account_cache.put(acc, to_auth_info(*db_acc), kAccountCacheTtl);
    return AccountRecord{db_acc->id, db_acc->acc};
  }
  return std::nullopt;
}

std::optional<AccountAuthInfo> get_account_auth(const std::string& acc) {
  if (const auto cached = g_account_cache.get(acc)) {
    return cached;
  }
  if (auto db_acc = db_find_account(acc)) {
    auto info = to_auth_info(*db_acc);
    g_account_cache.put(acc, info, kAccountCacheTtl);
    return info;
  }
  return std::nullopt;
}

std::optional<AccountRecord> create_account_with_password(const std::string& acc, const std::string& salt,
                                                          const std::string& hash) {
  if (auto db_acc = db_create_account(acc, salt, hash)) {
    g_account_cache.put(acc, to_auth_info(*db_acc), kAccountCacheTtl);
    return AccountRecord{db_acc->id, db_acc->acc};
  }
  return std::nullopt;
}

bool set_account_password(const std::string& acc, const std::string& salt, const std::string& hash) {
  const bool changed = db_set_password(acc, salt, hash);
  if (changed) {
    g_account_cache.erase(acc);
  }
  return changed;
}

bool is_bl_exempt(const std::string& acc) {
  if (auto info = get_account_auth(acc)) {
    return info->bl_exempt;
  }
  return false;
}

bool set_bl_exempt(const std::string& acc, bool exempt) {
  const bool changed = db_set_bl_exempt(acc, exempt);
  if (changed) {
    g_account_cache.erase(acc);
  }
  return changed;
}

bool save_cloud(const std::string& acc, int area, const std::string& save_json, const SaveMeta& meta) {
  std::lock_guard<std::mutex> lock(g_mu);
  db_ensure_account(acc);
  const int64_t now = std::time(nullptr);
  const int64_t save_time = meta.save_time > 0 ? meta.save_time : now;

  {
    std::ofstream blob(save_blob_path(acc, area), std::ios::binary | std::ios::trunc);
    if (!blob) {
      return false;
    }
    blob.write(save_json.data(), static_cast<std::streamsize>(save_json.size()));
    blob.flush();
    if (!blob) {
      return false;
    }
  }

  std::ofstream out(save_path(acc, area));
  if (!out) {
    return false;
  }
  nlohmann::json wrapper{
      {"updated_at", now},
      {"username", meta.username},
      {"lev", meta.lev},
      {"save_time", save_time},
      {"manual_upload_at", meta.manual_upload_at > 0 ? meta.manual_upload_at : now},
      {"manual_download_at", meta.manual_download_at},
      {"save_bytes", save_json.size()},
  };
  out << wrapper.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  out.flush();
  return static_cast<bool>(out);
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
    meta.manual_upload_at = j.value("manual_upload_at", static_cast<int64_t>(0));
    meta.manual_download_at = j.value("manual_download_at", static_cast<int64_t>(0));
    return meta;
  } catch (...) {
  }
  return std::nullopt;
}

bool mark_manual_download(const std::string& acc, int area, int64_t at) {
  std::lock_guard<std::mutex> lock(g_mu);
  const std::string path = save_path(acc, area);
  std::ifstream in(path);
  if (!in) {
    return false;
  }
  nlohmann::json j;
  try {
    in >> j;
  } catch (...) {
    return false;
  }
  in.close();
  j["manual_download_at"] = at > 0 ? at : std::time(nullptr);
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    return false;
  }
  out << j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  out.flush();
  return static_cast<bool>(out);
}

bool clear_manual_download(const std::string& acc, int area) {
  std::lock_guard<std::mutex> lock(g_mu);
  const std::string path = save_path(acc, area);
  std::ifstream in(path);
  if (!in) {
    return false;
  }
  nlohmann::json j;
  try {
    in >> j;
  } catch (...) {
    return false;
  }
  in.close();
  j["manual_download_at"] = 0;
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    return false;
  }
  out << j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  out.flush();
  return static_cast<bool>(out);
}

std::optional<std::string> load_cloud(const std::string& acc, int area) {
  std::lock_guard<std::mutex> lock(g_mu);
  {
    std::ifstream blob(save_blob_path(acc, area), std::ios::binary);
    if (blob) {
      std::string data((std::istreambuf_iterator<char>(blob)), std::istreambuf_iterator<char>());
      if (!data.empty()) {
        return data;
      }
    }
  }
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
  const fs::path saves_dir = fs::path(g_data_dir) / "saves";
  if (!fs::exists(saves_dir)) {
    return {};
  }

  struct Aggregate {
    SaveRecord record;
    bool has_sav_file = false;
  };
  std::map<std::pair<std::string, int>, Aggregate> grouped;

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
    const std::string extension = filename.substr(dot);
    if (extension != ".sav" && extension != ".json") {
      continue;
    }
    const std::string acc = filename.substr(0, us);
    int area = 0;
    try {
      area = std::stoi(filename.substr(us + 1, dot - us - 1));
    } catch (...) {
      continue;
    }

    auto& aggregate = grouped[{acc, area}];
    SaveRecord& rec = aggregate.record;
    rec.acc = acc;
    rec.area = area;

    if (extension == ".sav") {
      aggregate.has_sav_file = true;
      rec.has_blob = true;
      rec.size = static_cast<size_t>(entry.file_size());
      continue;
    }

    rec.has_meta = true;
    try {
      std::ifstream in(entry.path());
      nlohmann::json j;
      in >> j;
      rec.username = j.value("username", rec.username);
      rec.lev = j.value("lev", rec.lev);
      rec.updated_at = j.value("updated_at", rec.updated_at);

      // 兼容早期把存档正文直接放在 .json 的格式；新格式正文位于同名 .sav。
      if (j.contains("save")) {
        rec.has_blob = true;
        if (!aggregate.has_sav_file) {
          rec.size = j["save"].is_string() ? j["save"].get_ref<const std::string&>().size()
                                           : j["save"].dump().size();
        }
      }
    } catch (...) {
    }
  }

  std::vector<SaveRecord> out;
  out.reserve(grouped.size());
  for (auto& [key, aggregate] : grouped) {
    (void)key;
    if (aggregate.record.username.empty()) {
      aggregate.record.username = aggregate.record.acc;
    }
    out.push_back(std::move(aggregate.record));
  }
  std::stable_sort(out.begin(), out.end(), [](const SaveRecord& a, const SaveRecord& b) {
    if (a.updated_at != b.updated_at) {
      return a.updated_at > b.updated_at;
    }
    if (a.acc != b.acc) {
      return a.acc < b.acc;
    }
    return a.area < b.area;
  });
  return out;
}

bool delete_save(const std::string& acc, int area) {
  std::lock_guard<std::mutex> lock(g_mu);
  const bool a = fs::remove(save_path(acc, area));
  const bool b = fs::remove(save_blob_path(acc, area));
  return a || b;
}

bool delete_account(const std::string& acc) {
  if (!db_delete_account(acc)) {
    return false;
  }
  g_account_cache.erase(acc);

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
  return true;
}

ServerStats get_stats() {
  ServerStats stats;
  stats.account_count = db_account_count();
  stats.next_id = db_next_id_hint();
  stats.save_count = list_saves().size();
  return stats;
}

}  // namespace jh::storage
