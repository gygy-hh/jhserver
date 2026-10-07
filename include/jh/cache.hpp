#pragma once

#include <chrono>
#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <utility>

namespace jh {

template <typename Key, typename Value>
class TtlCache {
 public:
  using Clock = std::chrono::steady_clock;
  using Duration = Clock::duration;

  explicit TtlCache(size_t max_entries) : max_entries_(max_entries) {}

  std::optional<Value> get(const Key& key) {
    {
      std::shared_lock<std::shared_mutex> lock(mu_);
      const auto it = entries_.find(key);
      if (it == entries_.end()) {
        return std::nullopt;
      }
      if (it->second.expires_at > Clock::now()) {
        return it->second.value;
      }
    }

    std::unique_lock<std::shared_mutex> lock(mu_);
    const auto it = entries_.find(key);
    if (it != entries_.end()) {
      if (it->second.expires_at > Clock::now()) {
        return it->second.value;
      }
      entries_.erase(it);
    }
    return std::nullopt;
  }

  template <typename Rep, typename Period>
  void put(const Key& key, Value value, const std::chrono::duration<Rep, Period>& ttl) {
    std::unique_lock<std::shared_mutex> lock(mu_);
    if (max_entries_ == 0) {
      return;
    }
    if (entries_.find(key) == entries_.end() && entries_.size() >= max_entries_) {
      entries_.erase(entries_.begin());
    }
    entries_.insert_or_assign(
        key, Entry{std::move(value), Clock::now() + std::chrono::duration_cast<Duration>(ttl)});
  }

  void erase(const Key& key) {
    std::unique_lock<std::shared_mutex> lock(mu_);
    entries_.erase(key);
  }

  void clear() {
    std::unique_lock<std::shared_mutex> lock(mu_);
    entries_.clear();
  }

 private:
  struct Entry {
    Value value;
    Clock::time_point expires_at;
  };

  size_t max_entries_;
  std::map<Key, Entry> entries_;
  mutable std::shared_mutex mu_;
};

}  // namespace jh
