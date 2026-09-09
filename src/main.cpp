#include "jh/config.hpp"
#include "jh/admin.hpp"
#include "jh/auth.hpp"
#include "jh/db.hpp"
#include "jh/handlers.hpp"
#include "jh/mail.hpp"
#include "jh/save_crypto.hpp"
#include "jh/storage.hpp"
#include "jh/update.hpp"

#include <httplib.h>

#include <cstdio>
#include <filesystem>
#include <iostream>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <netdb.h>
#endif

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
  jh::mail::init(config.data_dir);
  jh::update::init(config.data_dir, config);

  jh::AuthConfig auth_cfg;
  auth_cfg.min_password_len = config.min_password_len;
  jh::auth_init(auth_cfg);

  httplib::Server server;
  server.set_read_timeout(1800, 0);
  server.set_write_timeout(1800, 0);
  server.set_keep_alive_timeout(180);
  server.set_payload_max_length(2ULL * 1024 * 1024 * 1024);
  server.set_pre_routing_handler([&config](const httplib::Request& req, httplib::Response& res) {
    if (req.path.rfind("/admin", 0) == 0) {
      const auto expected =
          httplib::make_basic_authentication_header(config.admin_acc, config.admin_psw).second;
      if (!req.has_header("Authorization") ||
          req.get_header_value("Authorization") != expected) {
        res.status = 401;
        res.set_header("WWW-Authenticate", R"(Basic realm="jh-admin", charset="UTF-8")");
        res.set_content(R"({"error":"admin authentication required"})",
                        "application/json; charset=utf-8");
        return httplib::Server::HandlerResponse::Handled;
      }
    }
    const bool is_stream_upload = req.path == "/admin/api/update/apk";
    if (!is_stream_upload && req.has_header("Content-Length")) {
      try {
        if (std::stoull(req.get_header_value("Content-Length")) > 64ULL * 1024 * 1024) {
          res.status = 413;
          res.set_content(R"({"error":"payload too large"})", "application/json; charset=utf-8");
          return httplib::Server::HandlerResponse::Handled;
        }
      } catch (...) {
        res.status = 400;
        return httplib::Server::HandlerResponse::Handled;
      }
    }
    return httplib::Server::HandlerResponse::Unhandled;
  });
  jh::admin::register_routes(server, config);
  jh::update::register_routes(server, config);
  jh::handlers::register_routes(server, config);

  const int bind_flags = AI_PASSIVE | AI_NUMERICHOST;
  if (!server.bind_to_port(config.host, config.port, bind_flags)) {
    std::cerr << "Failed to bind " << config.host << ":" << config.port << std::endl;
    std::perror("bind");
    return 1;
  }

  std::cout << "JH game server listening on http://" << config.host << ":" << config.port << "/\n"
            << "  Admin panel: http://127.0.0.1:" << config.port << "/admin\n"
            << "  game_version(ver): " << config.game_version << "\n"
            << "  mysql: " << config.mysql.host << ":" << config.mysql.port << "/" << config.mysql.database << "\n"
            << "  admin_acc: " << config.admin_acc << "\n"
            << std::flush;

  if (!server.listen_after_bind()) {
    std::cerr << "Failed to listen on " << config.host << ":" << config.port << std::endl;
    return 1;
  }
  return 0;
}
