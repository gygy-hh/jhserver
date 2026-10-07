#include "jh/mail.hpp"

#include "jh/crypto.hpp"
#include "jh/redis.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <sstream>
#include <thread>

namespace jh::mail {

namespace {

using json = nlohmann::json;

constexpr int kMailLifetimeSeconds = 2 * 60 * 60;
constexpr const char* kMailIndex = "MAIL_SET";
constexpr const char* kGlobalMailIndex = "GLOBAL_MAIL_SET";
constexpr const char* kGlobalScheduleIndex = "GLOBAL_MAIL_SCHEDULES";
constexpr const char* kMailPrefix = "initData:";

std::unique_ptr<redis::Client> g_redis;

struct SchedulerState {
  std::atomic<bool> stop{false};
  std::thread worker;

  ~SchedulerState() {
    stop.store(true);
    if (worker.joinable()) {
      worker.join();
    }
  }
};

SchedulerState g_scheduler;

int64_t now_milliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string format_activity_time(std::time_t timestamp) {
  const std::time_t china_timestamp = timestamp + 8 * 60 * 60;
  std::tm local{};
#ifdef _WIN32
  gmtime_s(&local, &china_timestamp);
#else
  gmtime_r(&china_timestamp, &local);
#endif
  std::ostringstream output;
  output << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
  return output.str();
}

bool valid_key_part(const std::string& value) {
  return !value.empty() && value.find(':') == std::string::npos;
}

std::string mail_key(const std::string& channel, const std::string& acc, int area) {
  return std::string(kMailPrefix) + channel + ":" + acc + ":" + std::to_string(area);
}

std::string global_mail_key(const std::string& channel) {
  return std::string(kMailPrefix) + channel + ":global";
}

bool parse_mail_key(const std::string& key, std::string& channel, std::string& acc, int& area) {
  if (key.rfind(kMailPrefix, 0) != 0) {
    return false;
  }
  const size_t channel_start = std::char_traits<char>::length(kMailPrefix);
  const size_t channel_end = key.find(':', channel_start);
  if (channel_end == std::string::npos) {
    return false;
  }
  const size_t acc_end = key.find(':', channel_end + 1);
  if (acc_end == std::string::npos || key.find(':', acc_end + 1) != std::string::npos) {
    return false;
  }
  channel = key.substr(channel_start, channel_end - channel_start);
  acc = key.substr(channel_end + 1, acc_end - channel_end - 1);
  if (!valid_key_part(channel) || !valid_key_part(acc)) {
    return false;
  }
  try {
    area = std::stoi(key.substr(acc_end + 1));
  } catch (...) {
    return false;
  }
  return area > 0;
}

json make_reward(const std::map<std::string, int>& items, const char* type) {
  json props = json::object();
  for (const auto& [prop_id, count] : items) {
    if (!prop_id.empty() && count > 0) {
      props[prop_id] = count;
    }
  }
  const std::time_t begin = std::time(nullptr);
  const std::time_t end = begin + kMailLifetimeSeconds;
  const std::string begin_at = format_activity_time(begin);
  const std::string end_at = format_activity_time(end);
  const std::string hd_data =
      json::array({json{{"gift", json{{"getProp", props}}}, {"money", 0}}}).dump();
  return json{
      {"channel", "all"},
      {"hdData", hd_data},
      {"isStop", 0},
      {"beginAt", begin_at},
      {"endAt", end_at},
      {"md", crypto::md5_hex(begin_at + end_at + hd_data + crypto::kMttSalt)},
      {"type", type},
  };
}

std::optional<MailEntry> parse_entry(const std::string& channel, const std::string& acc, int area,
                                     const json& huo_dong, const char* type = "s_goldSum") {
  if (!huo_dong.is_array()) {
    return std::nullopt;
  }
  for (const auto& activity : huo_dong) {
    if (!activity.is_object() || activity.value("type", "") != type) {
      continue;
    }
    MailEntry entry;
    entry.channel = channel;
    entry.acc = acc;
    entry.area = area;
    entry.begin_at = activity.value("beginAt", "");
    entry.end_at = activity.value("endAt", "");
    try {
      const json hd_data = json::parse(activity.value("hdData", "[]"));
      if (!hd_data.is_array() || hd_data.empty() || !hd_data[0].is_object() ||
          !hd_data[0].contains("gift")) {
        return std::nullopt;
      }
      json gift = hd_data[0]["gift"];
      if (gift.is_string()) {
        gift = json::parse(gift.get<std::string>());
      }
      if (!gift.is_object() || !gift.contains("getProp") || !gift["getProp"].is_object()) {
        return std::nullopt;
      }
      for (auto it = gift["getProp"].begin(); it != gift["getProp"].end(); ++it) {
        if (it.value().is_number_integer()) {
          const int64_t value = it.value().get<int64_t>();
          if (value > 0 && value <= std::numeric_limits<int>::max()) {
            entry.items[it.key()] = static_cast<int>(value);
          }
        }
      }
    } catch (...) {
      return std::nullopt;
    }
    return entry;
  }
  return std::nullopt;
}

void remove_index_member(const std::string& key) {
  if (g_redis) {
    g_redis->zrem(kMailIndex, key);
  }
}

void cleanup_expired_index() {
  if (g_redis) {
    g_redis->zremrangebyscore(kMailIndex, 0, now_milliseconds());
    g_redis->zremrangebyscore(kGlobalMailIndex, 0, now_milliseconds());
  }
}

std::string generate_schedule_id() {
  static std::atomic<uint64_t> counter{0};
  const uint64_t value =
      static_cast<uint64_t>(now_milliseconds()) * 1000ULL + (counter.fetch_add(1) % 1000ULL);
  return "gm_" + std::to_string(value);
}

std::optional<GlobalMailSchedule> parse_schedule(const json& value) {
  if (!value.is_object()) {
    return std::nullopt;
  }
  GlobalMailSchedule schedule;
  schedule.id = value.value("id", "");
  schedule.channel = value.value("channel", "");
  schedule.scheduled_at = value.value("scheduled_at", static_cast<int64_t>(0));
  schedule.created_at = value.value("created_at", static_cast<int64_t>(0));
  if (schedule.id.empty() || !valid_key_part(schedule.channel) || schedule.scheduled_at <= 0 ||
      !value.contains("items") || !value["items"].is_object()) {
    return std::nullopt;
  }
  for (auto it = value["items"].begin(); it != value["items"].end(); ++it) {
    if (it.value().is_number_integer()) {
      const int count = it.value().get<int>();
      if (count > 0) {
        schedule.items[it.key()] = count;
      }
    }
  }
  if (schedule.items.empty()) {
    return std::nullopt;
  }
  return schedule;
}

void scheduler_loop() {
  while (!g_scheduler.stop.load()) {
    if (g_redis) {
      const int64_t now = static_cast<int64_t>(std::time(nullptr));
      const auto due = g_redis->zrangebyscore(kGlobalScheduleIndex, 0, std::to_string(now));
      for (const auto& raw : due) {
        try {
          const json stored = json::parse(raw);
          const auto schedule = parse_schedule(stored);
          if (!schedule || !stored.contains("base_huo_dong") ||
              !g_redis->zrem(kGlobalScheduleIndex, raw)) {
            continue;
          }
          if (!send_global(schedule->channel, stored["base_huo_dong"], schedule->items)) {
            g_redis->zadd(kGlobalScheduleIndex, schedule->scheduled_at, raw);
            std::cerr << "[globalMailSchedule] retry id=" << schedule->id << std::endl;
          } else {
            std::cerr << "[globalMailSchedule] sent id=" << schedule->id
                      << " channel=" << schedule->channel << std::endl;
          }
        } catch (...) {
          g_redis->zrem(kGlobalScheduleIndex, raw);
        }
      }
    }
    for (int i = 0; i < 10 && !g_scheduler.stop.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

}  // namespace

bool init(const RedisConfig& config, std::string* error) {
  auto client = std::make_unique<redis::Client>(config);
  if (!client->ping()) {
    if (error) {
      *error = client->last_error();
    }
    return false;
  }
  g_redis = std::move(client);
  cleanup_expired_index();
  if (!g_scheduler.worker.joinable()) {
    g_scheduler.stop.store(false);
    g_scheduler.worker = std::thread(scheduler_loop);
  }
  return true;
}

bool send(const std::string& channel, const std::string& acc, int area,
          const json& base_huo_dong, const std::map<std::string, int>& items) {
  if (!g_redis || !valid_key_part(channel) || !valid_key_part(acc) || area <= 0 ||
      items.empty()) {
    return false;
  }

  json result = base_huo_dong.is_array() ? base_huo_dong : json::array();
  for (auto it = result.begin(); it != result.end();) {
    if (it->is_object() && it->value("type", "") == "s_goldSum") {
      it = result.erase(it);
    } else {
      ++it;
    }
  }
  const json reward = make_reward(items, "s_goldSum");
  const auto parsed = parse_entry(channel, acc, area, json::array({reward}));
  if (!parsed || parsed->items.empty()) {
    return false;
  }
  result.push_back(reward);

  cleanup_expired_index();
  const std::string key = mail_key(channel, acc, area);
  if (!g_redis->set_ex(key, result.dump(), kMailLifetimeSeconds)) {
    return false;
  }
  const int64_t expires_at = now_milliseconds() + kMailLifetimeSeconds * 1000LL;
  return g_redis->zadd(kMailIndex, expires_at, key);
}

bool send_global(const std::string& channel, const json& base_huo_dong,
                 const std::map<std::string, int>& items) {
  if (!g_redis || !valid_key_part(channel) || items.empty()) {
    return false;
  }
  json result = base_huo_dong.is_array() ? base_huo_dong : json::array();
  for (auto it = result.begin(); it != result.end();) {
    if (it->is_object() && it->value("type", "") == "s_goldSum") {
      it = result.erase(it);
    } else {
      ++it;
    }
  }
  const json reward = make_reward(items, "s_goldSum");
  const auto parsed = parse_entry(channel, "", 0, json::array({reward}));
  if (!parsed || parsed->items.empty()) {
    return false;
  }
  result.push_back(reward);

  cleanup_expired_index();
  const std::string key = global_mail_key(channel);
  if (!g_redis->set_ex(key, result.dump(), kMailLifetimeSeconds)) {
    return false;
  }
  const int64_t expires_at = now_milliseconds() + kMailLifetimeSeconds * 1000LL;
  return g_redis->zadd(kGlobalMailIndex, expires_at, key);
}

std::optional<json> get_huo_dong(const std::string& channel, const std::string& acc, int area) {
  if (!g_redis || !valid_key_part(channel) || !valid_key_part(acc) || area <= 0) {
    return std::nullopt;
  }
  const std::string key = mail_key(channel, acc, area);
  const auto value = g_redis->get(key);
  if (!value) {
    remove_index_member(key);
    return std::nullopt;
  }
  try {
    json result = json::parse(*value);
    if (!result.is_array()) {
      g_redis->del(key);
      remove_index_member(key);
      return std::nullopt;
    }
    return result;
  } catch (...) {
    g_redis->del(key);
    remove_index_member(key);
    return std::nullopt;
  }
}

std::optional<json> get_global_huo_dong(const std::string& channel) {
  if (!g_redis || !valid_key_part(channel)) {
    return std::nullopt;
  }
  const std::string key = global_mail_key(channel);
  const auto value = g_redis->get(key);
  if (!value) {
    g_redis->zrem(kGlobalMailIndex, key);
    return std::nullopt;
  }
  try {
    json result = json::parse(*value);
    if (!result.is_array()) {
      g_redis->del(key);
      g_redis->zrem(kGlobalMailIndex, key);
      return std::nullopt;
    }
    return result;
  } catch (...) {
    g_redis->del(key);
    g_redis->zrem(kGlobalMailIndex, key);
    return std::nullopt;
  }
}

std::optional<json> take_huo_dong(const std::string& channel, const std::string& acc, int area) {
  return get_huo_dong(channel, acc, area);
}

std::optional<json> take_global_huo_dong(const std::string& channel, const std::string& acc,
                                         int area) {
  if (!valid_key_part(acc) || area <= 0) {
    return std::nullopt;
  }
  return get_global_huo_dong(channel);
}

std::vector<MailEntry> list(const std::string& channel) {
  std::vector<MailEntry> entries;
  if (!g_redis || !valid_key_part(channel)) {
    return entries;
  }
  cleanup_expired_index();
  const auto keys = g_redis->zrangebyscore(kMailIndex, now_milliseconds(), "+inf");
  for (const auto& key : keys) {
    std::string key_channel;
    std::string acc;
    int area = 0;
    if (!parse_mail_key(key, key_channel, acc, area)) {
      remove_index_member(key);
      continue;
    }
    if (key_channel != channel) {
      continue;
    }
    const auto huo_dong = get_huo_dong(key_channel, acc, area);
    if (!huo_dong) {
      continue;
    }
    if (auto entry = parse_entry(key_channel, acc, area, *huo_dong)) {
      entries.push_back(std::move(*entry));
    } else {
      g_redis->del(key);
      remove_index_member(key);
    }
  }
  return entries;
}

std::optional<MailEntry> detail(const std::string& channel, const std::string& acc, int area) {
  const auto huo_dong = get_huo_dong(channel, acc, area);
  if (!huo_dong) {
    return std::nullopt;
  }
  return parse_entry(channel, acc, area, *huo_dong);
}

std::optional<MailEntry> global_detail(const std::string& channel) {
  const auto huo_dong = get_global_huo_dong(channel);
  if (!huo_dong) {
    return std::nullopt;
  }
  return parse_entry(channel, "", 0, *huo_dong);
}

bool remove(const std::string& channel, const std::string& acc, int area) {
  if (!g_redis || !valid_key_part(channel) || !valid_key_part(acc) || area <= 0) {
    return false;
  }
  const std::string key = mail_key(channel, acc, area);
  if (!g_redis->get(key)) {
    remove_index_member(key);
    return false;
  }
  const bool deleted = g_redis->del(key);
  const bool removed_from_index = g_redis->zrem(kMailIndex, key);
  return deleted && removed_from_index;
}

bool remove_global(const std::string& channel) {
  if (!g_redis || !valid_key_part(channel)) {
    return false;
  }
  const std::string key = global_mail_key(channel);
  if (!g_redis->get(key)) {
    g_redis->zrem(kGlobalMailIndex, key);
    return false;
  }
  const bool deleted = g_redis->del(key);
  const bool removed_from_index = g_redis->zrem(kGlobalMailIndex, key);
  return deleted && removed_from_index;
}

std::string schedule_global(const std::string& channel, const json& base_huo_dong,
                            const std::map<std::string, int>& items, int64_t scheduled_at) {
  if (!g_redis || !valid_key_part(channel) || items.empty() || scheduled_at <= 0) {
    return {};
  }
  json props = json::object();
  for (const auto& [prop_id, count] : items) {
    if (!prop_id.empty() && count > 0) {
      props[prop_id] = count;
    }
  }
  if (props.empty()) {
    return {};
  }
  const std::string id = generate_schedule_id();
  const json stored{
      {"id", id},
      {"channel", channel},
      {"items", props},
      {"scheduled_at", scheduled_at},
      {"created_at", static_cast<int64_t>(std::time(nullptr))},
      {"base_huo_dong", base_huo_dong.is_array() ? base_huo_dong : json::array()},
  };
  if (!g_redis->zadd(kGlobalScheduleIndex, scheduled_at, stored.dump())) {
    return {};
  }
  return id;
}

std::vector<GlobalMailSchedule> list_global_schedules(const std::string& channel) {
  std::vector<GlobalMailSchedule> schedules;
  if (!g_redis || !valid_key_part(channel)) {
    return schedules;
  }
  const auto stored = g_redis->zrangebyscore(kGlobalScheduleIndex, 0, "+inf");
  for (const auto& raw : stored) {
    try {
      const auto schedule = parse_schedule(json::parse(raw));
      if (schedule && schedule->channel == channel) {
        schedules.push_back(*schedule);
      }
    } catch (...) {
    }
  }
  std::sort(schedules.begin(), schedules.end(),
            [](const GlobalMailSchedule& left, const GlobalMailSchedule& right) {
              return left.scheduled_at < right.scheduled_at;
            });
  return schedules;
}

bool cancel_global_schedule(const std::string& id) {
  if (!g_redis || id.empty()) {
    return false;
  }
  const auto stored = g_redis->zrangebyscore(kGlobalScheduleIndex, 0, "+inf");
  for (const auto& raw : stored) {
    try {
      const auto schedule = parse_schedule(json::parse(raw));
      if (schedule && schedule->id == id) {
        return g_redis->zrem(kGlobalScheduleIndex, raw);
      }
    } catch (...) {
    }
  }
  return false;
}

}  // namespace jh::mail
