#pragma once

#include <cstdint>
#include <string>

namespace jh {

struct MysqlConfig {
  std::string host = "127.0.0.1";
  int port = 3306;
  std::string user = "root";
  std::string password;
  std::string database = "jh_game";
};

struct ServerConfig {
  std::string host = "0.0.0.0";
  int port = 8888;
  int game_version = 478;
  std::string plat = "ANDR";
  std::string data_dir = "data";
  std::string root_dir = ".";
  std::string public_url;
  std::string remote_doc_secret = "jh-doc-v1-8f3c1a6e5d9247b0";
  MysqlConfig mysql;
  std::string admin_acc = "19848015669";
  std::string admin_psw = "123456";
  int min_password_len = 6;
};

ServerConfig load_config(int argc, char** argv);

std::string find_resource_path(const std::string& relative_path);

}  // namespace jh
