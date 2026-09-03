#pragma once

#include <cstdint>
#include <string>

namespace jh {

struct AuthConfig {
  int min_password_len = 6;
};

enum class AuthResult {
  Ok,
  AccRequired,
  PswRequired,
  InvalidPhone,
  InvalidCredentials,
  PasswordTooShort,
  AccountExists,
};

struct AuthOutcome {
  AuthResult result = AuthResult::Ok;
  std::string message;
  uint32_t account_id = 0;
  bool is_new_account = false;
};

void auth_init(const AuthConfig& config);

AuthOutcome auth_send_sms_code(const std::string& phone);
AuthOutcome auth_login_phone(const std::string& phone, const std::string& psw);
AuthOutcome auth_login_mail(const std::string& acc, const std::string& psw);

bool auth_set_password(const std::string& acc, const std::string& new_psw, std::string& error);
bool auth_reset_password(const std::string& acc, const std::string& new_psw, std::string& error);

std::string auth_result_message(AuthResult result);

}  // namespace jh
