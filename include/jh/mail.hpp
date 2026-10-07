#pragma once

#include "jh/config.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace jh::mail {

struct MailEntry {
  std::string channel;
  std::string acc;
  int area = 1;
  std::map<std::string, int> items;
  std::string begin_at;
  std::string end_at;
};

struct GlobalMailSchedule {
  std::string id;
  std::string channel;
  std::map<std::string, int> items;
  int64_t scheduled_at = 0;
  int64_t created_at = 0;
};

bool init(const RedisConfig& config, std::string* error = nullptr);

bool send(const std::string& channel, const std::string& acc, int area,
          const nlohmann::json& base_huo_dong, const std::map<std::string, int>& items);
bool send_global(const std::string& channel, const nlohmann::json& base_huo_dong,
                 const std::map<std::string, int>& items);
std::optional<nlohmann::json> get_huo_dong(const std::string& channel,
                                          const std::string& acc, int area);
std::optional<nlohmann::json> get_global_huo_dong(const std::string& channel);
std::optional<nlohmann::json> take_huo_dong(const std::string& channel,
                                           const std::string& acc, int area);
std::optional<nlohmann::json> take_global_huo_dong(const std::string& channel,
                                                  const std::string& acc, int area);
std::vector<MailEntry> list(const std::string& channel);
std::optional<MailEntry> detail(const std::string& channel, const std::string& acc, int area);
std::optional<MailEntry> global_detail(const std::string& channel);
bool remove(const std::string& channel, const std::string& acc, int area);
bool remove_global(const std::string& channel);
std::string schedule_global(const std::string& channel, const nlohmann::json& base_huo_dong,
                            const std::map<std::string, int>& items, int64_t scheduled_at);
std::vector<GlobalMailSchedule> list_global_schedules(const std::string& channel);
bool cancel_global_schedule(const std::string& id);

}  // namespace jh::mail
