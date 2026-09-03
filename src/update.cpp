#include "jh/update.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace fs = std::filesystem;

namespace jh::update {

namespace {

using json = nlohmann::json;

std::mutex g_mu;
std::string g_data_dir;
std::string g_default_base_url;

std::string updates_dir() {
  return g_data_dir + "/updates";
}

std::string json_dir() {
  return updates_dir() + "/json";
}

std::string assets_dir() {
  return updates_dir() + "/assets";
}

std::string apk_dir() {
  return updates_dir() + "/apk";
}

std::string manifest_path() {
  return updates_dir() + "/manifest.json";
}

std::string trim_slash(std::string s) {
  while (!s.empty() && s.back() == '/') {
    s.pop_back();
  }
  return s;
}

bool is_safe_filename(const std::string& name) {
  if (name.empty() || name.size() > 128) {
    return false;
  }
  for (char c : name) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '_' || c == '-')) {
      return false;
    }
  }
  return name.find("..") == std::string::npos;
}

std::string resolve_apk_url(const Manifest& m, const std::string& base_url) {
  if (!m.apk_url.empty()) {
    return m.apk_url;
  }
  if (!m.apk_file.empty()) {
    return trim_slash(base_url) + "/update/apk/" + m.apk_file;
  }
  return "";
}

Manifest manifest_from_json(const json& j) {
  Manifest m;
  m.enabled = j.value("enabled", true);
  m.program_version = j.value("program_version", 520);
  m.min_program_version = j.value("min_program_version", 520);
  m.game_version = j.value("game_version", 478);
  m.json_version = j.value("json_version", 1);
  m.json_file = j.value("json_file", "1.json");
  m.asset_version = j.value("asset_version", 0);
  m.asset_file = j.value("asset_file", "");
  m.force_update = j.value("force_update", false);
  m.update_notice = j.value("update_notice", "");
  m.apk_url = j.value("apk_url", "");
  m.apk_file = j.value("apk_file", "");
  m.updated_at = j.value("updated_at", static_cast<int64_t>(0));
  if (j.contains("huo_dong") && j["huo_dong"].is_array()) {
    m.huo_dong = j["huo_dong"];
  } else if (j.contains("huoDong") && j["huoDong"].is_array()) {
    m.huo_dong = j["huoDong"];
  }
  if (j.contains("repair") && j["repair"].is_array()) {
    m.repair = j["repair"];
  }
  return m;
}

json manifest_to_json(const Manifest& m) {
  return json{
      {"enabled", m.enabled},
      {"program_version", m.program_version},
      {"min_program_version", m.min_program_version},
      {"game_version", m.game_version},
      {"json_version", m.json_version},
      {"json_file", m.json_file},
      {"asset_version", m.asset_version},
      {"asset_file", m.asset_file},
      {"force_update", m.force_update},
      {"update_notice", m.update_notice},
      {"apk_url", m.apk_url},
      {"apk_file", m.apk_file},
      {"huo_dong", m.huo_dong},
      {"repair", m.repair},
      {"updated_at", m.updated_at},
  };
}

Manifest load_manifest_unlocked() {
  Manifest m;
  std::ifstream in(manifest_path());
  if (!in) {
    return m;
  }
  try {
    json j;
    in >> j;
    m = manifest_from_json(j);
  } catch (...) {
  }
  return m;
}

json build_update_payload(const Manifest& m, int client_json_ver, int client_asset_ver, int client_prog_ver,
                          const std::string& base_url) {
  const std::string base = trim_slash(base_url);
  const std::string apk_url = resolve_apk_url(m, base);

  const bool json_update = m.enabled && m.json_version > client_json_ver;
  const bool asset_update = m.enabled && m.asset_version > client_asset_ver && !m.asset_file.empty();
  const bool program_too_old = m.enabled && client_prog_ver > 0 && client_prog_ver < m.min_program_version;
  const bool apk_update = m.enabled && !apk_url.empty() &&
                          (client_prog_ver <= 0 || client_prog_ver < m.program_version);
  const bool need_update =
      json_update || asset_update || apk_update || program_too_old || m.force_update;

  json out{
      {"need_update", need_update},
      {"json_update", json_update},
      {"asset_update", asset_update},
      {"apk_update", apk_update},
      {"force_update", m.force_update || program_too_old},
      {"program_version", m.program_version},
      {"min_program_version", m.min_program_version},
      {"game_version", m.game_version},
      {"json_version", m.json_version},
      {"asset_version", m.asset_version},
      {"update_notice", m.update_notice},
      {"apk_url", apk_url},
      {"apk_file", m.apk_file},
  };

  if (!m.json_file.empty()) {
    out["json_url"] = base + "/update/json/" + m.json_file;
  }
  if (!m.asset_file.empty()) {
    out["asset_url"] = base + "/update/assets/" + m.asset_file;
    out["asset_file"] = m.asset_file;
  }
  if (json_update && out.contains("json_url")) {
    out["json_download_url"] = out["json_url"];
  }
  if (asset_update && out.contains("asset_url")) {
    out["asset_download_url"] = out["asset_url"];
  }
  if (apk_update && !apk_url.empty()) {
    out["apk_download_url"] = apk_url;
  }
  return out;
}

void sync_json_download_file_unlocked(const Manifest& m) {
  if (m.json_file.empty()) {
    return;
  }
  json doc{
      {"huoDong", m.huo_dong.is_array() ? m.huo_dong : json::array()},
      {"version", m.json_version},
      {"update", build_update_payload(m, 0, 0, 0, g_default_base_url)},
  };
  fs::create_directories(json_dir());
  std::ofstream out(json_dir() + "/" + m.json_file);
  out << doc.dump(2);
}

void save_manifest_unlocked(const Manifest& m) {
  fs::create_directories(updates_dir());
  std::ofstream out(manifest_path());
  out << manifest_to_json(m).dump(2);
  sync_json_download_file_unlocked(m);
}

void ensure_defaults_unlocked() {
  fs::create_directories(json_dir());
  fs::create_directories(assets_dir());
  fs::create_directories(apk_dir());
  if (!fs::exists(manifest_path())) {
    Manifest m;
    m.updated_at = static_cast<int64_t>(std::time(nullptr));
    save_manifest_unlocked(m);
  }
  const std::string sample_json = json_dir() + "/1.json";
  if (!fs::exists(sample_json)) {
    Manifest m = load_manifest_unlocked();
    sync_json_download_file_unlocked(m);
  }
}

bool serve_binary_file(const fs::path& path, httplib::Response& res, const std::string& mime) {
  if (!fs::exists(path) || !fs::is_regular_file(path)) {
    return false;
  }
  std::ifstream in(path, std::ios::binary);
  std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  res.set_content(data, mime);
  return true;
}

}  // namespace

void init(const std::string& data_dir, const ServerConfig& config) {
  std::lock_guard<std::mutex> lock(g_mu);
  g_data_dir = data_dir;
  if (!config.public_url.empty()) {
    g_default_base_url = trim_slash(config.public_url);
  } else if (config.host == "0.0.0.0") {
    g_default_base_url = "http://127.0.0.1:" + std::to_string(config.port);
  } else {
    g_default_base_url = "http://" + config.host + ":" + std::to_string(config.port);
  }
  ensure_defaults_unlocked();
}

Manifest get_manifest() {
  std::lock_guard<std::mutex> lock(g_mu);
  return load_manifest_unlocked();
}

bool save_manifest(const Manifest& manifest) {
  std::lock_guard<std::mutex> lock(g_mu);
  Manifest m = manifest;
  m.updated_at = static_cast<int64_t>(std::time(nullptr));
  save_manifest_unlocked(m);
  return true;
}

json get_huo_dong() {
  const Manifest m = get_manifest();
  if (!m.huo_dong.is_array()) {
    return json::array();
  }
  return m.huo_dong;
}

json get_repair() {
  const Manifest m = get_manifest();
  if (!m.repair.is_array()) {
    return json::array();
  }
  return m.repair;
}

json get_client_update(int client_prog_ver, int client_json_ver, int client_asset_ver) {
  const Manifest m = get_manifest();
  return build_update_payload(m, client_json_ver, client_asset_ver, client_prog_ver, g_default_base_url);
}

std::string public_base_url(const httplib::Request& req, const ServerConfig& config) {
  if (!config.public_url.empty()) {
    return trim_slash(config.public_url);
  }
  const std::string host = req.get_header_value("Host");
  if (!host.empty()) {
    const std::string proto = req.get_header_value("X-Forwarded-Proto");
    if (proto == "https") {
      return "https://" + host;
    }
    return "http://" + host;
  }
  return g_default_base_url;
}

json build_check_response(int client_json_ver, int client_asset_ver, int client_prog_ver,
                          const std::string& base_url) {
  const Manifest m = get_manifest();
  return build_update_payload(m, client_json_ver, client_asset_ver, client_prog_ver, base_url);
}

std::optional<std::string> load_json_file(const std::string& filename) {
  if (!is_safe_filename(filename)) {
    return std::nullopt;
  }
  std::ifstream in(json_dir() + "/" + filename, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool save_json_file(const std::string& filename, const std::string& content) {
  if (!is_safe_filename(filename)) {
    return false;
  }
  fs::create_directories(json_dir());
  std::ofstream out(json_dir() + "/" + filename, std::ios::binary);
  if (!out) {
    return false;
  }
  out << content;
  return true;
}

std::vector<std::string> list_json_files() {
  std::vector<std::string> out;
  const fs::path dir = json_dir();
  if (!fs::exists(dir)) {
    return out;
  }
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (entry.is_regular_file()) {
      out.push_back(entry.path().filename().string());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::string> list_asset_files() {
  std::vector<std::string> out;
  const fs::path dir = assets_dir();
  if (!fs::exists(dir)) {
    return out;
  }
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (entry.is_regular_file()) {
      out.push_back(entry.path().filename().string());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::string> list_apk_files() {
  std::vector<std::string> out;
  const fs::path dir = apk_dir();
  if (!fs::exists(dir)) {
    return out;
  }
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (entry.is_regular_file()) {
      out.push_back(entry.path().filename().string());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

void register_routes(httplib::Server& server, const ServerConfig& config) {
  server.Get("/update/manifest", [&config](const httplib::Request& req, httplib::Response& res) {
    const Manifest m = get_manifest();
    json out = build_update_payload(m, 0, 0, 0, public_base_url(req, config));
    out["huo_dong"] = m.huo_dong;
    out["repair"] = m.repair;
    out["enabled"] = m.enabled;
    out["updated_at"] = m.updated_at;
    res.set_content(out.dump(2), "application/json; charset=utf-8");
  });

  server.Get("/update/check", [&config](const httplib::Request& req, httplib::Response& res) {
    int json_ver = 0;
    int asset_ver = 0;
    int prog_ver = 0;
    if (req.has_param("json_ver")) {
      json_ver = std::stoi(req.get_param_value("json_ver"));
    }
    if (req.has_param("asset_ver")) {
      asset_ver = std::stoi(req.get_param_value("asset_ver"));
    }
    if (req.has_param("prog_ver")) {
      prog_ver = std::stoi(req.get_param_value("prog_ver"));
    }
    const json out = build_check_response(json_ver, asset_ver, prog_ver, public_base_url(req, config));
    res.set_content(out.dump(2), "application/json; charset=utf-8");
  });

  server.Get(R"(/update/json/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
    const std::string name = req.matches[1];
    auto content = load_json_file(name);
    if (!content) {
      res.status = 404;
      res.set_content(R"({"error":"not found"})", "application/json; charset=utf-8");
      return;
    }
    res.set_content(*content, "application/json; charset=utf-8");
  });

  server.Get(R"(/update/assets/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
    const std::string name = req.matches[1];
    if (!is_safe_filename(name)) {
      res.status = 400;
      res.set_content(R"({"error":"bad filename"})", "application/json; charset=utf-8");
      return;
    }
    const fs::path path = fs::path(assets_dir()) / name;
    std::string mime = "application/octet-stream";
    if (name.size() >= 4 && name.substr(name.size() - 4) == ".zip") {
      mime = "application/zip";
    }
    if (!serve_binary_file(path, res, mime)) {
      res.status = 404;
      res.set_content(R"({"error":"not found"})", "application/json; charset=utf-8");
    }
  });

  server.Get(R"(/update/apk/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
    const std::string name = req.matches[1];
    if (!is_safe_filename(name)) {
      res.status = 400;
      res.set_content(R"({"error":"bad filename"})", "application/json; charset=utf-8");
      return;
    }
    const fs::path path = fs::path(apk_dir()) / name;
    if (!serve_binary_file(path, res, "application/vnd.android.package-archive")) {
      res.status = 404;
      res.set_content(R"({"error":"not found"})", "application/json; charset=utf-8");
    }
  });
}

void register_admin_routes(httplib::Server& server, const ServerConfig& config) {
  server.Get("/admin/api/update", [&config](const httplib::Request& req, httplib::Response& res) {
    const Manifest m = get_manifest();
    const std::string base = public_base_url(req, config);
    json out = manifest_to_json(m);
    out["json_files"] = json::array();
    out["asset_files"] = json::array();
    out["apk_files"] = json::array();
    for (const auto& name : list_json_files()) {
      out["json_files"].push_back(name);
    }
    for (const auto& name : list_asset_files()) {
      out["asset_files"].push_back(name);
    }
    for (const auto& name : list_apk_files()) {
      out["apk_files"].push_back(name);
    }
    out["public_base_url"] = base;
    out["json_url"] = m.json_file.empty() ? "" : base + "/update/json/" + m.json_file;
    out["asset_url"] = m.asset_file.empty() ? "" : base + "/update/assets/" + m.asset_file;
    out["apk_download_url"] = resolve_apk_url(m, base);
    out["check_url"] = base + "/update/check?json_ver=0&asset_ver=0&prog_ver=520";
    res.set_content(out.dump(2), "application/json; charset=utf-8");
  });

  server.Post("/admin/api/update", [](const httplib::Request& req, httplib::Response& res) {
    json body;
    try {
      body = req.body.empty() ? json::object() : json::parse(req.body);
    } catch (...) {
      res.status = 400;
      res.set_content(R"({"error":"invalid json"})", "application/json; charset=utf-8");
      return;
    }
    Manifest m = get_manifest();
    if (body.contains("enabled")) {
      m.enabled = body["enabled"].get<bool>();
    }
    if (body.contains("program_version")) {
      m.program_version = body["program_version"].get<int>();
    }
    if (body.contains("min_program_version")) {
      m.min_program_version = body["min_program_version"].get<int>();
    }
    if (body.contains("game_version")) {
      m.game_version = body["game_version"].get<int>();
    }
    if (body.contains("json_version")) {
      m.json_version = body["json_version"].get<int>();
    }
    if (body.contains("json_file")) {
      m.json_file = body["json_file"].get<std::string>();
    }
    if (body.contains("asset_version")) {
      m.asset_version = body["asset_version"].get<int>();
    }
    if (body.contains("asset_file")) {
      m.asset_file = body["asset_file"].get<std::string>();
    }
    if (body.contains("force_update")) {
      m.force_update = body["force_update"].get<bool>();
    }
    if (body.contains("update_notice")) {
      m.update_notice = body["update_notice"].get<std::string>();
    }
    if (body.contains("apk_url")) {
      m.apk_url = body["apk_url"].get<std::string>();
    }
    if (body.contains("apk_file")) {
      m.apk_file = body["apk_file"].get<std::string>();
    }
    if (body.contains("huo_dong") && body["huo_dong"].is_array()) {
      m.huo_dong = body["huo_dong"];
    } else if (body.contains("huoDong") && body["huoDong"].is_array()) {
      m.huo_dong = body["huoDong"];
    }
    if (body.contains("repair") && body["repair"].is_array()) {
      m.repair = body["repair"];
    }
    if (!save_manifest(m)) {
      res.status = 500;
      res.set_content(R"({"error":"save failed"})", "application/json; charset=utf-8");
      return;
    }
    res.set_content(R"({"ok":true})", "application/json; charset=utf-8");
  });

  server.Post("/admin/api/update/apk", [](const httplib::Request& req, httplib::Response& res) {
    if (!req.has_file("file")) {
      res.status = 400;
      res.set_content(R"({"error":"file required"})", "application/json; charset=utf-8");
      return;
    }
    const auto file = req.get_file_value("file");
    std::string filename = file.filename;
    if (filename.empty()) {
      filename = "game.apk";
    }
    const auto pos = filename.find_last_of("/\\");
    if (pos != std::string::npos) {
      filename = filename.substr(pos + 1);
    }
    if (!is_safe_filename(filename)) {
      res.status = 400;
      res.set_content(R"({"error":"bad filename"})", "application/json; charset=utf-8");
      return;
    }
    fs::create_directories(apk_dir());
    std::ofstream out(apk_dir() + "/" + filename, std::ios::binary);
    out.write(file.content.data(), static_cast<std::streamsize>(file.content.size()));
    if (!out) {
      res.status = 500;
      res.set_content(R"({"error":"save failed"})", "application/json; charset=utf-8");
      return;
    }

    Manifest m = get_manifest();
    m.apk_file = filename;
    if (req.has_file("program_version")) {
      m.program_version = std::stoi(req.get_file_value("program_version").content);
    } else {
      m.program_version += 1;
    }
    save_manifest(m);
    res.set_content(json{{"ok", true}, {"apk_file", filename}, {"program_version", m.program_version}}.dump(),
                    "application/json; charset=utf-8");
  });

  server.Get("/admin/api/update/json", [](const httplib::Request& req, httplib::Response& res) {
    std::string file = "1.json";
    if (req.has_param("file")) {
      file = req.get_param_value("file");
    }
    auto content = load_json_file(file);
    if (!content) {
      res.status = 404;
      res.set_content(R"({"error":"not found"})", "application/json; charset=utf-8");
      return;
    }
    res.set_content(json{{"file", file}, {"content", *content}}.dump(2), "application/json; charset=utf-8");
  });

  server.Post("/admin/api/update/json", [](const httplib::Request& req, httplib::Response& res) {
    json body;
    try {
      body = req.body.empty() ? json::object() : json::parse(req.body);
    } catch (...) {
      res.status = 400;
      res.set_content(R"({"error":"invalid json"})", "application/json; charset=utf-8");
      return;
    }
    const std::string file = body.value("file", "1.json");
    std::string content;
    if (body.contains("content") && body["content"].is_string()) {
      content = body["content"].get<std::string>();
    } else if (body.contains("json")) {
      content = body["json"].dump();
    } else {
      res.status = 400;
      res.set_content(R"({"error":"content or json required"})", "application/json; charset=utf-8");
      return;
    }
    if (!save_json_file(file, content)) {
      res.status = 400;
      res.set_content(R"({"error":"save failed"})", "application/json; charset=utf-8");
      return;
    }

    Manifest m = get_manifest();
    m.json_file = file;
    if (body.contains("json_version")) {
      m.json_version = body["json_version"].get<int>();
    }
    try {
      const json parsed = json::parse(content);
      if (parsed.contains("huoDong") && parsed["huoDong"].is_array()) {
        m.huo_dong = parsed["huoDong"];
      }
    } catch (...) {
    }
    save_manifest(m);
    res.set_content(json{{"ok", true}, {"file", file}}.dump(), "application/json; charset=utf-8");
  });
}

}  // namespace jh::update
