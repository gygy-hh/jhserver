#include "jh/config.hpp"



#include <filesystem>

#include <fstream>

#include <nlohmann/json.hpp>

#include <system_error>

#include <vector>



#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN

#define WIN32_LEAN_AND_MEAN

#endif

#include <windows.h>

#endif



namespace fs = std::filesystem;



namespace jh {



namespace {



std::string get_executable_dir() {

#ifdef _WIN32

  char buffer[MAX_PATH] = {};

  const DWORD len = GetModuleFileNameA(nullptr, buffer, MAX_PATH);

  if (len == 0 || len >= MAX_PATH) {

    return ".";

  }

  return fs::path(buffer).parent_path().string();

#else

  return fs::current_path().string();

#endif

}



std::vector<fs::path> search_roots() {

  std::vector<fs::path> roots;

  roots.push_back(fs::current_path());



  const fs::path exe_dir = get_executable_dir();

  roots.push_back(exe_dir);



  fs::path dir = exe_dir;

  for (int i = 0; i < 6; ++i) {

    dir = dir.parent_path();

    if (dir.empty() || dir == dir.parent_path()) {

      break;

    }

    roots.push_back(dir);

  }



  std::vector<fs::path> unique;

  for (const auto& root : roots) {

    bool seen = false;

    for (const auto& u : unique) {

      std::error_code ec;
      if (fs::equivalent(u, root, ec) || u == root) {

        seen = true;

        break;

      }

    }

    if (!seen) {

      unique.push_back(root);

    }

  }

  return unique;

}



std::string detect_project_root() {

  for (const auto& root : search_roots()) {

    const fs::path marker = root / "web" / "admin.html";

    if (fs::exists(marker)) {

      return root.string();

    }

  }

  return fs::current_path().string();

}



fs::path make_absolute_data_dir(const std::string& data_dir, const std::string& root_dir) {

  fs::path p(data_dir);

  if (p.is_absolute()) {

    return p;

  }

  return fs::path(root_dir) / p;

}



}  // namespace



std::string find_resource_path(const std::string& relative_path) {

  for (const auto& root : search_roots()) {

    const fs::path candidate = root / relative_path;

    if (fs::exists(candidate)) {

      return candidate.string();

    }

  }

  return relative_path;

}



ServerConfig load_config(int argc, char** argv) {

  ServerConfig cfg;

  std::string config_path = "config.json";



  for (int i = 1; i + 1 < argc; ++i) {

    if (std::string(argv[i]) == "--config") {

      config_path = argv[i + 1];

    }

  }



  const std::string resolved_config = find_resource_path(config_path);

  cfg.root_dir = detect_project_root();



  std::ifstream in(resolved_config);

  if (in) {

    try {

      nlohmann::json j;

      in >> j;

      if (j.contains("host")) {

        cfg.host = j["host"].get<std::string>();

      }

      if (j.contains("port")) {

        cfg.port = j["port"].get<int>();

      }

      if (j.contains("game_version")) {

        cfg.game_version = j["game_version"].get<int>();

      }

      if (j.contains("plat")) {

        cfg.plat = j["plat"].get<std::string>();

      }

      if (j.contains("data_dir")) {

        cfg.data_dir = j["data_dir"].get<std::string>();

      }

      if (j.contains("public_url")) {

        cfg.public_url = j["public_url"].get<std::string>();

      }

      if (j.contains("admin_acc")) {
        cfg.admin_acc = j["admin_acc"].get<std::string>();
      }
      if (j.contains("admin_psw")) {
        cfg.admin_psw = j["admin_psw"].get<std::string>();
      }
      if (j.contains("mysql") && j["mysql"].is_object()) {
        const auto& mysql = j["mysql"];
        if (mysql.contains("host")) {
          cfg.mysql.host = mysql["host"].get<std::string>();
        }
        if (mysql.contains("port")) {
          cfg.mysql.port = mysql["port"].get<int>();
        }
        if (mysql.contains("user")) {
          cfg.mysql.user = mysql["user"].get<std::string>();
        }
        if (mysql.contains("password")) {
          cfg.mysql.password = mysql["password"].get<std::string>();
        }
        if (mysql.contains("database")) {
          cfg.mysql.database = mysql["database"].get<std::string>();
        }
      }
      if (j.contains("min_password_len")) {
        cfg.min_password_len = j["min_password_len"].get<int>();
      }

    } catch (...) {

    }

  }



  cfg.data_dir = make_absolute_data_dir(cfg.data_dir, cfg.root_dir).string();



  for (int i = 1; i + 1 < argc; ++i) {

    if (std::string(argv[i]) == "--port") {

      cfg.port = std::stoi(argv[i + 1]);

    } else if (std::string(argv[i]) == "--version") {

      cfg.game_version = std::stoi(argv[i + 1]);

    } else if (std::string(argv[i]) == "--host") {

      cfg.host = argv[i + 1];

    }

  }



  return cfg;

}



}  // namespace jh

