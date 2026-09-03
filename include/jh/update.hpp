#pragma once

#include "jh/config.hpp"

#include <httplib.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace jh::update {

struct Manifest {
  bool enabled = true;
  int program_version = 520;
  int min_program_version = 520;
  int game_version = 478;
  int json_version = 1;
  std::string json_file = "1.json";
  int asset_version = 0;
  std::string asset_file;
  bool force_update = false;
  std::string update_notice;
  std::string apk_url;
  std::string apk_file;
  nlohmann::json huo_dong = nlohmann::json::array();
  nlohmann::json repair = nlohmann::json::array();
  int64_t updated_at = 0;
};

void init(const std::string& data_dir, const ServerConfig& config);

Manifest get_manifest();
bool save_manifest(const Manifest& manifest);

nlohmann::json get_huo_dong();
nlohmann::json get_repair();
nlohmann::json get_client_update(int client_prog_ver, int client_json_ver, int client_asset_ver);

std::string public_base_url(const httplib::Request& req, const ServerConfig& config);
nlohmann::json build_check_response(int client_json_ver, int client_asset_ver, int client_prog_ver,
                                    const std::string& base_url);

std::optional<std::string> load_json_file(const std::string& filename);
bool save_json_file(const std::string& filename, const std::string& content);

std::vector<std::string> list_json_files();
std::vector<std::string> list_asset_files();
std::vector<std::string> list_apk_files();

void register_routes(httplib::Server& server, const ServerConfig& config);
void register_admin_routes(httplib::Server& server, const ServerConfig& config);

}  // namespace jh::update
