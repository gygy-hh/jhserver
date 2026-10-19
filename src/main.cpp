#include "jh/config.hpp"
#include "jh/auth.hpp"
#include "jh/db.hpp"
#include "jh/gateway.hpp"
#include "jh/lunjian.hpp"
#include "jh/mail.hpp"
#include "jh/save_crypto.hpp"
#include "jh/storage.hpp"
#include "jh/update.hpp"

#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
  const jh::ServerConfig config = jh::load_config(argc, argv);
  fs::create_directories(config.data_dir);

  const std::string legacy_accounts = config.data_dir + "/accounts.json";
  if (!jh::db_init(config.mysql, config.admin_acc, config.admin_psw, legacy_accounts)) {
    std::cerr << "Failed to connect MySQL. Check config.json mysql section.\n";
    return 1;
  }

  jh::storage::init(config.data_dir);
  jh::save_crypto::init(config.data_dir);
  std::string redis_error;
  if (!jh::mail::init(config.redis, &redis_error)) {
    std::cerr << "Failed to connect Redis: " << redis_error << "\n";
    return 1;
  }
  jh::update::init(config.data_dir, config);
  jh::lunjian::init(config.data_dir);

  jh::AuthConfig auth_cfg;
  auth_cfg.min_password_len = config.min_password_len;
  jh::auth_init(auth_cfg);

  return jh::gateway::serve(config);
}
