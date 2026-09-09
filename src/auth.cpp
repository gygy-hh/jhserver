#include "jh/auth.hpp"

#include "jh/crypto.hpp"
#include "jh/db.hpp"
#include "jh/session.hpp"

#include <chrono>
#include <random>

namespace jh {

namespace {

AuthConfig g_config;

bool constant_time_eq(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) {
    return false;
  }
  unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  }
  return diff == 0;
}

std::string random_salt() {
  static thread_local std::mt19937 rng(static_cast<unsigned>(
      std::chrono::steady_clock::now().time_since_epoch().count()));
  static const char kHex[] = "0123456789abcdef";
  std::string out(16, '\0');
  for (char& c : out) {
    c = kHex[rng() % 16];
  }
  return out;
}

std::string hash_password(const std::string& salt, const std::string& psw) {
  return crypto::md5_hex(salt + ":" + psw);
}

AuthOutcome make_outcome(AuthResult result, const std::string& message = "") {
  AuthOutcome out;
  out.result = result;
  out.message = message.empty() ? auth_result_message(result) : message;
  return out;
}

bool is_valid_cn_mobile(const std::string& phone) {
  if (phone.size() != 11 || phone[0] != '1') {
    return false;
  }
  for (char c : phone) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  return true;
}

bool is_valid_mail_acc(const std::string& acc) {
  return acc.find('@') != std::string::npos && acc.size() >= 5;
}

AuthOutcome login_existing(const std::string& acc, const std::string& psw) {
  auto account = db_find_account(acc);
  if (!account) {
    return make_outcome(AuthResult::InvalidCredentials);
  }
  if (!account->has_password()) {
    return make_outcome(AuthResult::InvalidCredentials);
  }
  if (static_cast<int>(psw.size()) < g_config.min_password_len) {
    return make_outcome(AuthResult::PasswordTooShort);
  }
  const std::string expected = hash_password(account->psw_salt, psw);
  if (!constant_time_eq(expected, account->psw_hash)) {
    return make_outcome(AuthResult::InvalidCredentials);
  }
  AuthOutcome out = make_outcome(AuthResult::Ok);
  out.account_id = account->id;
  return out;
}

AuthOutcome register_account(const std::string& acc, const std::string& psw) {
  if (db_find_account(acc)) {
    return make_outcome(AuthResult::AccountExists);
  }
  if (static_cast<int>(psw.size()) < g_config.min_password_len) {
    return make_outcome(AuthResult::PasswordTooShort);
  }
  const std::string salt = random_salt();
  const std::string hash = hash_password(salt, psw);
  auto created = db_create_account(acc, salt, hash);
  if (!created) {
    return make_outcome(AuthResult::AccountExists);
  }
  AuthOutcome out = make_outcome(AuthResult::Ok);
  out.account_id = created->id;
  out.is_new_account = true;
  return out;
}

}  // namespace

void auth_init(const AuthConfig& config) {
  g_config = config;
}

AuthOutcome auth_send_sms_code(const std::string& phone) {
  if (!is_valid_cn_mobile(phone)) {
    return make_outcome(AuthResult::InvalidPhone);
  }
  return make_outcome(AuthResult::Ok);
}

AuthOutcome auth_login_phone(const std::string& phone, const std::string& psw) {
  if (phone.empty()) {
    return make_outcome(AuthResult::AccRequired);
  }
  if (psw.empty()) {
    return make_outcome(AuthResult::PswRequired);
  }
  if (!is_valid_cn_mobile(phone)) {
    return make_outcome(AuthResult::InvalidPhone);
  }
  if (!db_find_account(phone)) {
    return register_account(phone, psw);
  }
  return login_existing(phone, psw);
}

AuthOutcome auth_login_mail(const std::string& acc, const std::string& psw) {
  if (acc.empty()) {
    return make_outcome(AuthResult::AccRequired);
  }
  if (psw.empty()) {
    return make_outcome(AuthResult::PswRequired);
  }
  if (!is_valid_mail_acc(acc)) {
    return make_outcome(AuthResult::InvalidCredentials, "invalid mail");
  }
  if (!db_find_account(acc)) {
    return register_account(acc, psw);
  }
  return login_existing(acc, psw);
}

bool auth_set_password(const std::string& acc, const std::string& new_psw, std::string& error) {
  if (!db_find_account(acc)) {
    db_ensure_account(acc);
  }
  if (static_cast<int>(new_psw.size()) < g_config.min_password_len) {
    error = "password too short";
    return false;
  }
  const std::string salt = random_salt();
  const std::string hash = hash_password(salt, new_psw);
  if (!db_set_password(acc, salt, hash)) {
    error = "save failed";
    return false;
  }
  session_revoke_account(acc);
  return true;
}

bool auth_reset_password(const std::string& acc, const std::string& new_psw, std::string& error) {
  return auth_set_password(acc, new_psw, error);
}

std::string auth_result_message(AuthResult result) {
  switch (result) {
    case AuthResult::Ok:
      return "ok";
    case AuthResult::AccRequired:
      return "acc required";
    case AuthResult::PswRequired:
      return "psw required";
    case AuthResult::InvalidPhone:
      return "invalid phone";
    case AuthResult::InvalidCredentials:
      return "invalid credentials";
    case AuthResult::PasswordTooShort:
      return "password too short";
    case AuthResult::AccountExists:
      return "account exists";
    default:
      return "auth failed";
  }
}

}  // namespace jh
