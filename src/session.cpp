#include "jh/session.hpp"

#include "jh/cache.hpp"
#include "jh/crypto.hpp"
#include "jh/db.hpp"

#include <array>
#include <chrono>
#include <random>

namespace jh {

namespace {

constexpr int64_t kSessionTtlSeconds = 30LL * 24 * 60 * 60;
TtlCache<std::string, std::string> g_session_cache(200000);

int64_t now_sec() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string random_token() {
  std::random_device random;
  std::array<unsigned char, 32> bytes{};
  for (auto& byte : bytes) {
    byte = static_cast<unsigned char>(random());
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string token(bytes.size() * 2, '\0');
  for (size_t i = 0; i < bytes.size(); ++i) {
    token[i * 2] = kHex[bytes[i] >> 4];
    token[i * 2 + 1] = kHex[bytes[i] & 0x0f];
  }
  return token;
}

bool is_token_format(const std::string& token) {
  if (token.size() != 64) {
    return false;
  }
  for (const unsigned char c : token) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

}  // namespace

std::optional<IssuedSession> session_issue(const std::string& acc) {
  if (acc.empty()) {
    return std::nullopt;
  }
  for (int attempt = 0; attempt < 3; ++attempt) {
    IssuedSession issued;
    issued.token = random_token();
    issued.expires_at = now_sec() + kSessionTtlSeconds;
    const std::string token_hash = crypto::md5_hex(issued.token);
    if (db_insert_session(acc, token_hash, issued.expires_at)) {
      g_session_cache.put(token_hash, acc, std::chrono::seconds(kSessionTtlSeconds));
      return issued;
    }
  }
  return std::nullopt;
}

std::optional<std::string> session_validate(const std::string& token) {
  if (!is_token_format(token)) {
    return std::nullopt;
  }
  const std::string token_hash = crypto::md5_hex(token);
  if (const auto cached = g_session_cache.get(token_hash)) {
    return cached;
  }
  const auto session = db_find_session(token_hash);
  if (!session) {
    return std::nullopt;
  }
  const int64_t remaining = session->expires_at - now_sec();
  if (remaining <= 0) {
    return std::nullopt;
  }
  g_session_cache.put(token_hash, session->acc, std::chrono::seconds(remaining));
  return session->acc;
}

void session_revoke_account(const std::string& acc) {
  db_revoke_sessions(acc);
  g_session_cache.clear();
}

}  // namespace jh
