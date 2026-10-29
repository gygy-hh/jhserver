#pragma once

#include "jh/config.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jh::push_hub {

struct OnlineClient {
  std::string acc;
  std::string peer;
  std::string map;
  std::string nick;
  int x = 0;
  int y = 0;
  int64_t connected_at = 0;
  int64_t last_active = 0;
};

// 在独立端口启动 TCP 推送监听；push_port<=0 时为 no-op。
bool start(const ServerConfig& config);
void stop();

std::size_t online_count();
std::vector<OnlineClient> list_online();

// 向指定账号推送一条验证消息；成功返回 true。
bool push_to_acc(const std::string& acc, const std::string& msg, std::string* error = nullptr);
std::size_t broadcast(const std::string& msg);

}  // namespace jh::push_hub
