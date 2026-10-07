#pragma once

#include "jh/config.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace jh::redis {

class Client {
 public:
  explicit Client(RedisConfig config);
  ~Client();

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  bool ping();
  std::optional<std::string> get(const std::string& key);
  bool set_ex(const std::string& key, const std::string& value, int ttl_seconds);
  bool set_nx_ex(const std::string& key, const std::string& value, int ttl_seconds);
  bool del(const std::string& key);
  bool zadd(const std::string& key, int64_t score, const std::string& member);
  bool zrem(const std::string& key, const std::string& member);
  bool zremrangebyscore(const std::string& key, int64_t min_score, int64_t max_score);
  std::vector<std::string> zrangebyscore(const std::string& key, int64_t min_score,
                                        const std::string& max_score);

  const std::string& last_error() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace jh::redis
