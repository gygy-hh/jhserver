#include "jh/mail.hpp"

#include "jh/save_blob.hpp"
#include "jh/save_crypto.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>

namespace fs = std::filesystem;

namespace jh::mail {

namespace {

using json = nlohmann::json;

std::mutex g_mu;
std::string g_data_dir;

std::string safe_acc(const std::string& acc) {
  std::string safe = acc;
  for (char& c : safe) {
    if (c == '/' || c == '\\' || c == ':') {
      c = '_';
    }
  }
  return safe;
}

std::string mail_path(const std::string& acc, int area) {
  return g_data_dir + "/mails/" + safe_acc(acc) + "_" + std::to_string(area) + ".json";
}

std::string legacy_mail_path(const std::string& acc) {
  return g_data_dir + "/mails/" + safe_acc(acc) + ".json";
}

json load_file_unlocked(const std::string& acc, int area) {
  json j = json{{"acc", acc}, {"area", area}, {"mails", json::array()}};
  const std::string path = mail_path(acc, area);
  std::ifstream in(path);
  if (!in && area == 1) {
    in.open(legacy_mail_path(acc));
  }
  if (in) {
    try {
      in >> j;
    } catch (...) {
    }
  }
  if (!j.contains("mails") || !j["mails"].is_array()) {
    j["mails"] = json::array();
  }
  j["acc"] = acc;
  j["area"] = area;
  return j;
}

void save_file_unlocked(const std::string& acc, int area, const json& j) {
  fs::create_directories(g_data_dir + "/mails");
  json out = j;
  out["acc"] = acc;
  out["area"] = area;
  std::ofstream out_file(mail_path(acc, area));
  out_file << out.dump(2);
}

MailRecord from_json(const json& m, int area) {
  MailRecord rec;
  rec.id = m.value("id", "");
  rec.desp = m.value("desp", "");
  rec.area = m.value("area", area);
  rec.created_at = m.value("created_at", static_cast<int64_t>(0));
  rec.status = m.value("status", "pending");
  if (m.contains("items") && m["items"].is_object()) {
    for (auto it = m["items"].begin(); it != m["items"].end(); ++it) {
      rec.items[it.key()] = it.value().get<int>();
    }
  }
  return rec;
}

std::string gen_mail_id() {
  const int now = static_cast<int>(std::time(nullptr));
  static thread_local std::mt19937 rng{static_cast<unsigned>(
      std::chrono::steady_clock::now().time_since_epoch().count())};
  const int rnd = static_cast<int>(rng() % 100000);
  std::ostringstream oss;
  oss << "2_" << now << "_" << rnd;
  return oss.str();
}

std::optional<json> read_mygift_from_blob(const std::string& blob, int save_index) {
  auto dat_cipher = save_blob::get_segment(blob, "dat.json");
  if (!dat_cipher || dat_cipher->empty() || *dat_cipher == "null") {
    return json::object();
  }
  try {
    const std::string plain = save_crypto::decrypt_dat_auto(*dat_cipher, save_index);
    const json doc = json::parse(plain);
    if (doc.contains("myGift") && doc["myGift"].is_object()) {
      return doc["myGift"];
    }
    return json::object();
  } catch (...) {
    return std::nullopt;
  }
}

}  // namespace

void init(const std::string& data_dir) {
  g_data_dir = data_dir;
  fs::create_directories(data_dir + "/mails");
}

std::string send(const std::string& acc, int area, const std::string& desp,
                 const std::map<std::string, int>& items) {
  const int resolved_area = area > 0 ? area : 1;
  std::lock_guard<std::mutex> lock(g_mu);
  auto j = load_file_unlocked(acc, resolved_area);
  const std::string id = gen_mail_id();
  json entry{
      {"id", id},
      {"desp", desp},
      {"area", resolved_area},
      {"items", json::object()},
      {"created_at", static_cast<int64_t>(std::time(nullptr))},
      {"status", "pending"},
  };
  for (const auto& [prop_id, count] : items) {
    entry["items"][prop_id] = count;
  }
  j["mails"].push_back(entry);
  save_file_unlocked(acc, resolved_area, j);
  return id;
}

std::vector<MailRecord> list(const std::string& acc, int area) {
  const int resolved_area = area > 0 ? area : 1;
  std::lock_guard<std::mutex> lock(g_mu);
  std::vector<MailRecord> out;
  const auto j = load_file_unlocked(acc, resolved_area);
  for (const auto& m : j["mails"]) {
    out.push_back(from_json(m, resolved_area));
  }
  return out;
}

bool remove(const std::string& acc, int area, const std::string& mail_id) {
  const int resolved_area = area > 0 ? area : 1;
  std::lock_guard<std::mutex> lock(g_mu);
  auto j = load_file_unlocked(acc, resolved_area);
  auto& mails = j["mails"];
  for (size_t i = 0; i < mails.size(); ++i) {
    if (mails[i].value("id", "") == mail_id && mails[i].value("status", "") == "pending") {
      mails.erase(i);
      save_file_unlocked(acc, resolved_area, j);
      return true;
    }
  }
  return false;
}

void remove_account(const std::string& acc) {
  std::lock_guard<std::mutex> lock(g_mu);
  const std::string safe = safe_acc(acc);
  const fs::path dir = g_data_dir + "/mails";
  if (!fs::exists(dir)) {
    return;
  }
  for (const auto& entry : fs::directory_iterator(dir)) {
    const std::string name = entry.path().filename().string();
    if (name == safe + ".json" || name.rfind(safe + "_", 0) == 0) {
      fs::remove(entry.path());
    }
  }
}

std::vector<MailRecord> pending_for_injection(const std::string& acc, int area) {
  const int resolved_area = area > 0 ? area : 1;
  std::lock_guard<std::mutex> lock(g_mu);
  std::vector<MailRecord> out;
  const auto j = load_file_unlocked(acc, resolved_area);
  for (const auto& m : j["mails"]) {
    if (m.value("status", "") == "pending") {
      out.push_back(from_json(m, resolved_area));
    }
  }
  return out;
}

void mark_in_save(const std::string& acc, int area, const std::vector<std::string>& mail_ids) {
  if (mail_ids.empty()) {
    return;
  }
  const int resolved_area = area > 0 ? area : 1;
  std::lock_guard<std::mutex> lock(g_mu);
  auto j = load_file_unlocked(acc, resolved_area);
  for (auto& m : j["mails"]) {
    const std::string id = m.value("id", "");
    for (const auto& target : mail_ids) {
      if (id == target && m.value("status", "") == "pending") {
        m["status"] = "in_save";
        m["in_save_at"] = static_cast<int64_t>(std::time(nullptr));
      }
    }
  }
  save_file_unlocked(acc, resolved_area, j);
}

void sync_claimed_from_save(const std::string& acc, int area, int save_index, const std::string& save_blob) {
  const int resolved_area = area > 0 ? area : 1;
  const auto mygift = read_mygift_from_blob(save_blob, save_index);
  if (!mygift) {
    return;
  }

  std::lock_guard<std::mutex> lock(g_mu);
  auto j = load_file_unlocked(acc, resolved_area);
  bool changed = false;
  for (auto& m : j["mails"]) {
    if (m.value("status", "") != "in_save") {
      continue;
    }
    const std::string id = m.value("id", "");
    if (!mygift->contains(id)) {
      m["status"] = "claimed";
      m["claimed_at"] = static_cast<int64_t>(std::time(nullptr));
      changed = true;
    }
  }
  if (changed) {
    save_file_unlocked(acc, resolved_area, j);
  }
}

size_t pending_count(const std::string& acc, int area) {
  const int resolved_area = area > 0 ? area : 1;
  std::lock_guard<std::mutex> lock(g_mu);
  const auto j = load_file_unlocked(acc, resolved_area);
  size_t n = 0;
  for (const auto& m : j["mails"]) {
    if (m.value("status", "") == "pending") {
      ++n;
    }
  }
  return n;
}

}  // namespace jh::mail
