#include "jh/crypto.hpp"
#include "jh/mail.hpp"

#include <chrono>
#include <ctime>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace {

using json = nlohmann::json;

bool require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    return false;
  }
  return true;
}

const json* personal_reward(const json& activities) {
  const json* found = nullptr;
  for (const auto& activity : activities) {
    if (activity.is_object() && activity.value("type", "") == "s_goldSum") {
      if (found) {
        return nullptr;
      }
      found = &activity;
    }
  }
  return found;
}

}  // namespace

int main(int argc, char** argv) {
  jh::RedisConfig config;
  config.port = argc > 1 ? std::atoi(argv[1]) : 16379;
  config.password = "test-password";
  config.database = 2;
  config.connect_timeout_ms = 1000;
  std::string error;
  const bool initialized = jh::mail::init(config, &error);
  if (!require(initialized, error.c_str())) {
    return 1;
  }

  const json base = json::array({
      json{{"type", "gg"}, {"channel", "all"}, {"hdData", "notice"}, {"isStop", 0}},
      json{{"type", "s_goldSum"}, {"channel", "all"}, {"hdData", "old"}, {"isStop", 0}},
  });
  if (!require(jh::mail::send("none", "13800138000", 1, base, {{"9716", 10}}),
               "first send")) {
    return 1;
  }
  auto activities = jh::mail::get_huo_dong("none", "13800138000", 1);
  if (!require(activities && activities->size() == 2, "stored full activity list")) {
    return 1;
  }
  const json* reward = personal_reward(*activities);
  if (!require(reward != nullptr, "exactly one s_goldSum")) {
    return 1;
  }
  const std::string hd_data = reward->value("hdData", "");
  const std::string expected_md = jh::crypto::md5_hex(
      reward->value("beginAt", "") + reward->value("endAt", "") + hd_data +
      jh::crypto::kMttSalt);
  if (!require(reward->value("md", "") == expected_md, "s_goldSum md")) {
    return 1;
  }

  if (!require(jh::mail::send("none", "13800138000", 1, base, {{"42", 3}}),
               "overwrite send")) {
    return 1;
  }
  const auto detail = jh::mail::detail("none", "13800138000", 1);
  if (!require(detail && detail->items == std::map<std::string, int>{{"42", 3}},
               "later send replaces earlier props")) {
    return 1;
  }
  if (!require(jh::mail::take_huo_dong("none", "13800138000", 1).has_value(),
               "personal mail first delivery") ||
      !require(jh::mail::take_huo_dong("none", "13800138000", 1).has_value(),
               "personal mail remains available during login delivery window")) {
    return 1;
  }
  if (!require(jh::mail::send("other", "13800138000", 1, base, {{"7", 1}}),
               "channel-isolated send")) {
    return 1;
  }
  const auto none_entries = jh::mail::list("none");
  const auto other_entries = jh::mail::list("other");
  if (!require(none_entries.size() == 1 && other_entries.size() == 1,
               "channel-isolated index listing")) {
    return 1;
  }
  if (!require(jh::mail::send_global("none", base, {{"88", 5}}), "global send")) {
    return 1;
  }
  const auto global_activities = jh::mail::get_global_huo_dong("none");
  int s_gold_sum_count = 0;
  bool global_gift_is_object = false;
  if (global_activities) {
    for (const auto& activity : *global_activities) {
      if (activity.value("type", "") == "s_goldSum") {
        ++s_gold_sum_count;
        const json data = json::parse(activity.value("hdData", "[]"));
        global_gift_is_object =
            data.is_array() && !data.empty() && data[0].value("gift", json{}).is_object();
      }
    }
  }
  if (!require(s_gold_sum_count == 1 && global_gift_is_object,
               "global mail uses one client-compatible s_goldSum")) {
    return 1;
  }
  const auto global = jh::mail::global_detail("none");
  if (!require(global && global->items == std::map<std::string, int>{{"88", 5}},
               "global mail has no account or area")) {
    return 1;
  }
  if (!require(jh::mail::take_global_huo_dong("none", "13800138000", 1).has_value(),
               "global mail first delivery") ||
      !require(jh::mail::take_global_huo_dong("none", "13800138000", 1).has_value(),
               "global mail remains available during login delivery window") ||
      !require(jh::mail::take_global_huo_dong("none", "13900139000", 1).has_value(),
               "global mail remains available to another player")) {
    return 1;
  }
  if (!require(jh::mail::remove("none", "13800138000", 1), "delete mail") ||
      !require(!jh::mail::get_huo_dong("none", "13800138000", 1), "deleted mail absent")) {
    return 1;
  }
  if (!require(jh::mail::get_global_huo_dong("none").has_value(),
               "personal deletion preserves global mail") ||
      !require(jh::mail::remove_global("none"), "delete global mail") ||
      !require(!jh::mail::get_global_huo_dong("none"), "deleted global mail absent")) {
    return 1;
  }
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  const std::string cancelled_id =
      jh::mail::schedule_global("none", base, {{"99", 2}}, now + 60);
  if (!require(!cancelled_id.empty(), "create global schedule") ||
      !require(jh::mail::list_global_schedules("none").size() == 1,
               "list global schedule") ||
      !require(jh::mail::cancel_global_schedule(cancelled_id), "cancel global schedule") ||
      !require(jh::mail::list_global_schedules("none").empty(),
               "cancelled schedule absent")) {
    return 1;
  }
  const std::string due_id = jh::mail::schedule_global("none", base, {{"100", 4}}, now + 1);
  if (!require(!due_id.empty(), "create due global schedule")) {
    return 1;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(2200));
  const auto scheduled_global = jh::mail::global_detail("none");
  if (!require(scheduled_global &&
                   scheduled_global->items == std::map<std::string, int>{{"100", 4}},
               "scheduler activates global mail") ||
      !require(jh::mail::list_global_schedules("none").empty(),
               "sent schedule removed")) {
    return 1;
  }

  std::cout << "mail redis integration passed\n";
  return 0;
}
