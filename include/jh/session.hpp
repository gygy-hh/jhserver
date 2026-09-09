#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace jh {

struct IssuedSession {
  std::string token;
  int64_t expires_at = 0;
};

std::optional<IssuedSession> session_issue(const std::string& acc);
std::optional<std::string> session_validate(const std::string& token);
void session_revoke_account(const std::string& acc);

}  // namespace jh
