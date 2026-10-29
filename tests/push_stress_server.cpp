// 推送端口压测用：只启动 push_hub，session 校验换成桩（任意 64 位 hex 即通过），不连 MySQL/Redis。
#include "jh/config.hpp"
#include "jh/push_hub.hpp"
#include "jh/session.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace jh {

std::optional<std::string> session_validate(const std::string& token) {
  if (token.size() != 64) {
    return std::nullopt;
  }
  return "stress_" + token.substr(0, 12);
}

}  // namespace jh

namespace {

long read_status_kb(const char* key) {
  std::ifstream in("/proc/self/status");
  std::string k;
  while (in >> k) {
    if (k == key) {
      long v = 0;
      in >> v;
      return v;
    }
    std::string rest;
    std::getline(in, rest);
  }
  return -1;
}

}  // namespace

int main(int argc, char** argv) {
  jh::ServerConfig cfg;
  cfg.host = "0.0.0.0";
  cfg.push_port = argc > 1 ? std::atoi(argv[1]) : 28081;
  const int broadcast_every = argc > 2 ? std::atoi(argv[2]) : 10;
  cfg.push_max_conns = argc > 3 ? std::atoi(argv[3]) : 1500;
  if (!jh::push_hub::start(cfg)) {
    return 1;
  }
  int tick = 0;
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    ++tick;
    std::cout << "[stat] t=" << tick << " online=" << jh::push_hub::online_count()
              << " threads=" << read_status_kb("Threads:") << " rss_kb=" << read_status_kb("VmRSS:")
              << std::endl;
    if (broadcast_every > 0 && tick % broadcast_every == 0) {
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
      const auto sent = jh::push_hub::broadcast("ts=" + std::to_string(ms));
      const auto cost = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count() -
                        ms;
      std::cout << "[broadcast] sent=" << sent << " cost_ms=" << cost << std::endl;
    }
  }
}
