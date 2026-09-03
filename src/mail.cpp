#include "jh/mail.hpp"

#include "jh/save_blob.hpp"
#include "jh/save_crypto.hpp"
#include "jh/storage.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace jh::mail {

namespace {

using json = nlohmann::json;

std::mutex g_mu;
std::mutex g_broadcast_mu;
std::string g_data_dir;

std::string broadcast_path() {
  return g_data_dir + "/broadcast_jobs.json";
}

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

int64_t next_push_version(const json& mails, int64_t now) {
  int64_t version = now;
  for (const auto& mail : mails) {
    version =
        std::max(version, mail.value("push_version", mail.value("created_at", static_cast<int64_t>(0))) + 1);
  }
  return version;
}

bool send_unlocked_with_id(const std::string& acc, int area, const std::string& id, const std::string& desp,
                           const std::map<std::string, int>& items) {
  auto j = load_file_unlocked(acc, area);
  for (const auto& existing : j["mails"]) {
    if (existing.value("id", "") == id) {
      return true;
    }
  }
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  json entry{
      {"id", id},
      {"desp", desp},
      {"area", area},
      {"items", json::object()},
      {"created_at", now},
      {"push_version", next_push_version(j["mails"], now)},
      {"status", "pending"},
  };
  for (const auto& [prop_id, count] : items) {
    entry["items"][prop_id] = count;
  }
  j["mails"].push_back(std::move(entry));
  save_file_unlocked(acc, area, j);
  return true;
}

json load_broadcasts_unlocked() {
  json root{{"jobs", json::array()}};
  std::ifstream in(broadcast_path());
  if (in) {
    try {
      in >> root;
    } catch (...) {
    }
  }
  if (!root.contains("jobs") || !root["jobs"].is_array()) {
    root["jobs"] = json::array();
  }
  return root;
}

void save_broadcasts_unlocked(const json& root) {
  std::ofstream out(broadcast_path());
  out << root.dump(2);
}

MailRecord from_json(const json& m, int area) {
  MailRecord rec;
  rec.id = m.value("id", "");
  rec.desp = m.value("desp", "");
  rec.area = m.value("area", area);
  rec.created_at = m.value("created_at", static_cast<int64_t>(0));
  rec.push_version = m.value("push_version", rec.created_at);
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

void broadcast_loop();

}  // namespace

void init(const std::string& data_dir) {
  g_data_dir = data_dir;
  fs::create_directories(data_dir + "/mails");
  std::thread(broadcast_loop).detach();
}

std::string send(const std::string& acc, int area, const std::string& desp,
                 const std::map<std::string, int>& items) {
  const int resolved_area = area > 0 ? area : 1;
  std::lock_guard<std::mutex> lock(g_mu);
  const std::string id = gen_mail_id();
  send_unlocked_with_id(acc, resolved_area, id, desp, items);
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

void mark_pushed(const std::string& acc, int area, const std::vector<std::string>& mail_ids) {
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
        m["status"] = "pushed";
        m["pushed_at"] = static_cast<int64_t>(std::time(nullptr));
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

std::string create_broadcast(const std::string& desp, const std::map<std::string, int>& items,
                             int64_t scheduled_at) {
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  if (scheduled_at <= 0) {
    scheduled_at = now;
  }
  const std::string id = "b_" + gen_mail_id().substr(2);
  std::lock_guard<std::mutex> lock(g_broadcast_mu);
  auto root = load_broadcasts_unlocked();
  json item_json = json::object();
  for (const auto& [prop_id, count] : items) {
    item_json[prop_id] = count;
  }
  root["jobs"].push_back({
      {"id", id},
      {"desp", desp},
      {"items", item_json},
      {"scheduled_at", scheduled_at},
      {"created_at", now},
      {"completed_at", 0},
      {"recipient_count", 0},
      {"sent_count", 0},
      {"status", "scheduled"},
  });
  save_broadcasts_unlocked(root);
  return id;
}

std::vector<BroadcastJob> list_broadcasts() {
  std::lock_guard<std::mutex> lock(g_broadcast_mu);
  const auto root = load_broadcasts_unlocked();
  std::vector<BroadcastJob> out;
  for (const auto& j : root["jobs"]) {
    BroadcastJob job;
    job.id = j.value("id", "");
    job.desp = j.value("desp", "");
    job.scheduled_at = j.value("scheduled_at", static_cast<int64_t>(0));
    job.created_at = j.value("created_at", static_cast<int64_t>(0));
    job.completed_at = j.value("completed_at", static_cast<int64_t>(0));
    job.recipient_count = j.value("recipient_count", static_cast<size_t>(0));
    job.sent_count = j.value("sent_count", static_cast<size_t>(0));
    job.status = j.value("status", "scheduled");
    if (j.contains("items") && j["items"].is_object()) {
      for (auto it = j["items"].begin(); it != j["items"].end(); ++it) {
        job.items[it.key()] = it.value().get<int>();
      }
    }
    out.push_back(std::move(job));
  }
  std::reverse(out.begin(), out.end());
  return out;
}

bool cancel_broadcast(const std::string& job_id) {
  std::lock_guard<std::mutex> lock(g_broadcast_mu);
  auto root = load_broadcasts_unlocked();
  for (auto& j : root["jobs"]) {
    if (j.value("id", "") == job_id && j.value("status", "") == "scheduled") {
      j["status"] = "cancelled";
      j["completed_at"] = static_cast<int64_t>(std::time(nullptr));
      save_broadcasts_unlocked(root);
      return true;
    }
  }
  return false;
}

namespace {

void broadcast_loop() {
  while (true) {
    json selected;
    {
      std::lock_guard<std::mutex> lock(g_broadcast_mu);
      auto root = load_broadcasts_unlocked();
      const int64_t now = static_cast<int64_t>(std::time(nullptr));
      for (auto& j : root["jobs"]) {
        const std::string status = j.value("status", "");
        if ((status == "scheduled" || status == "running") &&
            j.value("scheduled_at", static_cast<int64_t>(0)) <= now) {
          j["status"] = "running";
          selected = j;
          save_broadcasts_unlocked(root);
          break;
        }
      }
    }

    if (selected.is_null()) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }

    std::map<std::string, int> items;
    for (auto it = selected["items"].begin(); it != selected["items"].end(); ++it) {
      items[it.key()] = it.value().get<int>();
    }
    std::set<std::pair<std::string, int>> recipients;
    for (const auto& save : storage::list_saves()) {
      recipients.emplace(save.acc, save.area);
    }

    size_t sent_count = 0;
    const std::string job_id = selected.value("id", "");
    const std::string mail_id = "broadcast_" + job_id;
    for (const auto& [acc, area] : recipients) {
      std::lock_guard<std::mutex> lock(g_mu);
      if (send_unlocked_with_id(acc, area, mail_id, selected.value("desp", "系统奖励"), items)) {
        ++sent_count;
      }
    }

    {
      std::lock_guard<std::mutex> lock(g_broadcast_mu);
      auto root = load_broadcasts_unlocked();
      for (auto& j : root["jobs"]) {
        if (j.value("id", "") == job_id) {
          j["status"] = "completed";
          j["recipient_count"] = recipients.size();
          j["sent_count"] = sent_count;
          j["completed_at"] = static_cast<int64_t>(std::time(nullptr));
          break;
        }
      }
      save_broadcasts_unlocked(root);
    }
    std::cerr << "[broadcast] id=" << job_id << " recipients=" << recipients.size()
              << " sent=" << sent_count << std::endl;
  }
}

}  // namespace

}  // namespace jh::mail
