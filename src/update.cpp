#include "jh/update.hpp"

#include "jh/crypto.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>

#include <zlib.h>

namespace fs = std::filesystem;

namespace jh::update {

namespace {

using json = nlohmann::json;

std::mutex g_mu;
std::string g_data_dir;
std::string g_default_base_url;
std::mutex g_doc_nonce_mu;
std::unordered_map<std::string, int64_t> g_doc_nonces;

constexpr char kDocXorKey[] = "nnjttjmbjyzmmht91";
constexpr size_t kMaxDocBytes = 32u * 1024u * 1024u;

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

std::string integrity_dir() {
  return updates_dir() + "/integrity";
}

std::string conf_dir() {
  return updates_dir() + "/conf";
}

std::string doc_path() {
  return conf_dir() + "/doc.json";
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

bool refresh_apk_integrity_unlocked(Manifest& m) {
  m.apk_md5.clear();
  m.apk_size = 0;
  if (!is_safe_filename(m.integrity_apk_file)) {
    return false;
  }
  const fs::path path = fs::path(integrity_dir()) / m.integrity_apk_file;
  if (!fs::exists(path) || !fs::is_regular_file(path)) {
    return false;
  }
  const std::string md5 = crypto::md5_file(path.string());
  if (md5.size() != 32) {
    return false;
  }
  m.apk_md5 = md5;
  m.apk_size = fs::file_size(path);
  return true;
}

void doc_xor_transform(std::string& data) {
  constexpr size_t key_len = sizeof(kDocXorKey) - 1;
  for (size_t i = 0; i < data.size(); ++i) {
    const unsigned char k = static_cast<unsigned char>(kDocXorKey[i % key_len]);
    const unsigned char b = static_cast<unsigned char>(data[i]);
    data[i] = static_cast<char>(k ^ ~(k ^ b));
  }
}

bool normalize_doc_content(const std::string& input, std::string& encrypted) {
  if (input.empty() || input.size() > kMaxDocBytes) {
    return false;
  }
  size_t first = 0;
  while (first < input.size() && std::isspace(static_cast<unsigned char>(input[first])) != 0) {
    ++first;
  }
  std::string plain;
  if (first < input.size() && (input[first] == '{' || input[first] == '[')) {
    plain = input;
    encrypted = input;
    doc_xor_transform(encrypted);
  } else {
    encrypted = input;
    plain = input;
    doc_xor_transform(plain);
  }
  try {
    const json parsed = json::parse(plain);
    return parsed.is_object();
  } catch (...) {
    encrypted.clear();
    return false;
  }
}

bool write_doc_unlocked(const std::string& encrypted) {
  fs::create_directories(conf_dir());
  const std::string temp_path = doc_path() + ".tmp";
  {
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    out.write(encrypted.data(), static_cast<std::streamsize>(encrypted.size()));
    out.flush();
    if (!out) {
      return false;
    }
  }
  std::error_code ec;
  fs::rename(temp_path, doc_path(), ec);
  if (!ec) {
    return true;
  }
  fs::remove(doc_path(), ec);
  ec.clear();
  fs::rename(temp_path, doc_path(), ec);
  if (ec) {
    fs::remove(temp_path, ec);
    return false;
  }
  return true;
}

bool refresh_doc_metadata_unlocked(Manifest& m) {
  m.doc_md5.clear();
  m.doc_size = 0;
  const fs::path path(doc_path());
  if (!fs::exists(path) || !fs::is_regular_file(path)) {
    return false;
  }
  m.doc_md5 = crypto::md5_file(path.string());
  if (m.doc_md5.size() != 32) {
    m.doc_md5.clear();
    return false;
  }
  m.doc_size = fs::file_size(path);
  return m.doc_size > 0 && m.doc_size <= kMaxDocBytes;
}

std::optional<std::string> load_doc_unlocked() {
  std::ifstream in(doc_path(), std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (content.empty() || content.size() > kMaxDocBytes) {
    return std::nullopt;
  }
  return content;
}

bool is_hex_token(const std::string& value, size_t min_size, size_t max_size) {
  if (value.size() < min_size || value.size() > max_size) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return std::isxdigit(c) != 0;
  });
}

bool constant_time_equal(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) {
    return false;
  }
  unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  }
  return diff == 0;
}

bool authorize_doc_request(const httplib::Request& req, const ServerConfig& config,
                           std::string& timestamp, std::string& nonce) {
  if (config.remote_doc_secret.size() < 16 || !req.has_param("ts") ||
      !req.has_param("nonce") || !req.has_param("sig")) {
    return false;
  }
  timestamp = req.get_param_value("ts");
  nonce = req.get_param_value("nonce");
  const std::string signature = req.get_param_value("sig");
  if (!is_hex_token(nonce, 16, 64) || !is_hex_token(signature, 32, 32)) {
    return false;
  }
  int64_t ts = 0;
  try {
    ts = std::stoll(timestamp);
  } catch (...) {
    return false;
  }
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  if (ts < now - 300 || ts > now + 300) {
    return false;
  }
  const std::string expected =
      crypto::md5_hex(config.remote_doc_secret + "|" + timestamp + "|" + nonce + "|doc");
  if (!constant_time_equal(expected, signature)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_doc_nonce_mu);
  for (auto it = g_doc_nonces.begin(); it != g_doc_nonces.end();) {
    if (it->second < now - 300) {
      it = g_doc_nonces.erase(it);
    } else {
      ++it;
    }
  }
  if (g_doc_nonces.find(nonce) != g_doc_nonces.end()) {
    return false;
  }
  g_doc_nonces[nonce] = now;
  return true;
}

uint8_t hex_byte(const std::string& hex, size_t pos) {
  auto nibble = [](char c) -> uint8_t {
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
    return static_cast<uint8_t>(c - 'A' + 10);
  };
  return static_cast<uint8_t>((nibble(hex[pos]) << 4) | nibble(hex[pos + 1]));
}

void crypt_doc_transport(std::string& content, const std::string& secret,
                         const std::string& timestamp, const std::string& nonce) {
  const std::string digest = crypto::md5_hex(secret + "|" + timestamp + "|" + nonce + "|stream");
  std::array<uint8_t, 16> key{};
  for (size_t i = 0; i < key.size(); ++i) {
    key[i] = hex_byte(digest, i * 2);
  }
  for (size_t i = 0; i < content.size(); ++i) {
    content[i] =
        static_cast<char>(static_cast<unsigned char>(content[i]) ^ key[i % key.size()]);
  }
}

bool compress_doc_transport(const std::string& input, std::string& output) {
  uLongf compressed_size = compressBound(static_cast<uLong>(input.size()));
  output.resize(static_cast<size_t>(compressed_size));
  const int rc = compress2(reinterpret_cast<Bytef*>(output.data()), &compressed_size,
                           reinterpret_cast<const Bytef*>(input.data()),
                           static_cast<uLong>(input.size()), Z_BEST_COMPRESSION);
  if (rc != Z_OK) {
    output.clear();
    return false;
  }
  output.resize(static_cast<size_t>(compressed_size));
  return true;
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
  m.integrity_apk_file = j.value("integrity_apk_file", "");
  m.apk_md5 = j.value("apk_md5", "");
  m.apk_size = j.value("apk_size", static_cast<uint64_t>(0));
  m.doc_version = j.value("doc_version", 0);
  m.doc_md5 = j.value("doc_md5", "");
  m.doc_size = j.value("doc_size", static_cast<uint64_t>(0));
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
      {"integrity_apk_file", m.integrity_apk_file},
      {"apk_md5", m.apk_md5},
      {"apk_size", m.apk_size},
      {"doc_version", m.doc_version},
      {"doc_md5", m.doc_md5},
      {"doc_size", m.doc_size},
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
      {"apk_md5", m.apk_md5},
      {"apk_size", m.apk_size},
      {"doc_version", m.doc_version},
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

bool save_manifest_unlocked(const Manifest& m) {
  std::error_code dir_ec;
  fs::create_directories(updates_dir(), dir_ec);
  if (dir_ec) {
    return false;
  }
  const std::string temp_path = manifest_path() + ".tmp";
  {
    std::ofstream out(temp_path, std::ios::trunc);
    out << manifest_to_json(m).dump(2);
    out.flush();
    if (!out) {
      return false;
    }
  }
  std::error_code ec;
  fs::rename(temp_path, manifest_path(), ec);
  if (ec) {
    // Windows rename cannot overwrite; Linux uses atomic replace above.
    std::error_code fallback_ec;
    fs::remove(manifest_path(), fallback_ec);
    fs::rename(temp_path, manifest_path(), fallback_ec);
    if (fallback_ec) {
      fs::remove(temp_path, fallback_ec);
      return false;
    }
  }
  sync_json_download_file_unlocked(m);
  return true;
}

void ensure_defaults_unlocked() {
  fs::create_directories(json_dir());
  fs::create_directories(assets_dir());
  fs::create_directories(apk_dir());
  fs::create_directories(integrity_dir());
  fs::create_directories(conf_dir());
  if (!fs::exists(manifest_path())) {
    Manifest m;
    m.updated_at = static_cast<int64_t>(std::time(nullptr));
    save_manifest_unlocked(m);
  } else {
    Manifest m = load_manifest_unlocked();
    bool migrated = false;
    if (m.integrity_apk_file.empty() && !m.apk_md5.empty() && is_safe_filename(m.apk_file)) {
      const fs::path old_path = fs::path(apk_dir()) / m.apk_file;
      const fs::path new_path = fs::path(integrity_dir()) / m.apk_file;
      std::error_code ec;
      if (fs::exists(old_path) && fs::is_regular_file(old_path)) {
        fs::copy_file(old_path, new_path, fs::copy_options::overwrite_existing, ec);
        if (!ec) {
          m.integrity_apk_file = m.apk_file;
          migrated = refresh_apk_integrity_unlocked(m);
        }
      }
    } else if (!m.integrity_apk_file.empty() && m.apk_md5.empty()) {
      migrated = refresh_apk_integrity_unlocked(m);
    }
    if (fs::exists(doc_path()) && (m.doc_md5.empty() || m.doc_size == 0)) {
      migrated = refresh_doc_metadata_unlocked(m) || migrated;
    }
    if (migrated) {
      m.updated_at = static_cast<int64_t>(std::time(nullptr));
      save_manifest_unlocked(m);
    }
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

struct StreamUpload {
  std::string filename;
  fs::path path;
  uint64_t size = 0;
};

std::optional<StreamUpload> receive_stream_upload(const httplib::Request& req, httplib::Response& res,
                                                  const httplib::ContentReader& content_reader,
                                                  const fs::path& dir, const std::string& default_name) {
  if (req.is_multipart_form_data()) {
    res.status = 415;
    res.set_content(R"({"error":"please refresh admin page and retry"})",
                    "application/json; charset=utf-8");
    return std::nullopt;
  }
  std::string filename = req.has_param("filename") ? req.get_param_value("filename") : default_name;
  const auto pos = filename.find_last_of("/\\");
  if (pos != std::string::npos) {
    filename = filename.substr(pos + 1);
  }
  if (!is_safe_filename(filename)) {
    res.status = 400;
    res.set_content(R"({"error":"bad filename"})", "application/json; charset=utf-8");
    return std::nullopt;
  }

  fs::create_directories(dir);
  const fs::path final_path = dir / filename;
  const fs::path temp_path = final_path.string() + ".upload";
  std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
  if (!out) {
    res.status = 500;
    res.set_content(R"({"error":"open failed"})", "application/json; charset=utf-8");
    return std::nullopt;
  }

  constexpr uint64_t kMaxApkBytes = 2ULL * 1024 * 1024 * 1024;
  uint64_t size = 0;
  bool too_large = false;
  const bool received = content_reader([&](const char* data, size_t length) {
    if (length > kMaxApkBytes - size) {
      too_large = true;
      return false;
    }
    out.write(data, static_cast<std::streamsize>(length));
    if (!out) {
      return false;
    }
    size += length;
    return true;
  });
  out.flush();
  const bool write_ok = static_cast<bool>(out);
  out.close();
  if (!received || !write_ok || size == 0) {
    std::error_code ec;
    fs::remove(temp_path, ec);
    res.status = too_large ? 413 : 400;
    res.set_content(too_large ? R"({"error":"APK exceeds 2GB"})" : R"({"error":"upload failed"})",
                    "application/json; charset=utf-8");
    return std::nullopt;
  }

  std::error_code ec;
  fs::rename(temp_path, final_path, ec);
  if (ec) {
    std::error_code fallback_ec;
    fs::remove(final_path, fallback_ec);
    fs::rename(temp_path, final_path, fallback_ec);
    if (fallback_ec) {
      fs::remove(temp_path, fallback_ec);
      res.status = 500;
      res.set_content(R"({"error":"save failed"})", "application/json; charset=utf-8");
      return std::nullopt;
    }
  }
  return StreamUpload{filename, final_path, size};
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
  return save_manifest_unlocked(m);
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

  server.Get("/update/conf/doc", [&config](const httplib::Request& req, httplib::Response& res) {
    std::string timestamp;
    std::string nonce;
    if (!authorize_doc_request(req, config, timestamp, nonce)) {
      res.status = 403;
      res.set_content(R"({"error":"forbidden"})", "application/json; charset=utf-8");
      return;
    }

    std::optional<std::string> content;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      content = load_doc_unlocked();
    }
    if (!content) {
      res.status = 404;
      res.set_content(R"({"error":"doc not configured"})", "application/json; charset=utf-8");
      return;
    }

    const Manifest m = get_manifest();
    const std::string doc_md5 = crypto::md5_hex(*content);
    const std::string size = std::to_string(content->size());
    const std::string response_signature = crypto::md5_hex(
        config.remote_doc_secret + "|" + timestamp + "|" + nonce + "|" + doc_md5 + "|" + size);
    std::string compressed;
    if (!compress_doc_transport(*content, compressed)) {
      res.status = 500;
      res.set_content(R"({"error":"compression failed"})", "application/json; charset=utf-8");
      return;
    }
    crypt_doc_transport(compressed, config.remote_doc_secret, timestamp, nonce);
    res.set_header("Cache-Control", "no-store");
    res.set_header("X-Doc-MD5", doc_md5);
    res.set_header("X-Doc-Version", std::to_string(m.doc_version));
    res.set_header("X-Doc-Size", size);
    res.set_header("X-Doc-Encoding", "deflate");
    res.set_header("X-Doc-Signature", response_signature);
    res.set_content(std::move(compressed), "application/octet-stream");
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
    out["doc_configured"] = !m.doc_md5.empty() && m.doc_size > 0;
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

  server.Post("/admin/api/update/apk",
              [](const httplib::Request& req, httplib::Response& res,
                 const httplib::ContentReader& content_reader) {
    const auto upload =
        receive_stream_upload(req, res, content_reader, fs::path(apk_dir()), "game.apk");
    if (!upload) {
      return;
    }
    Manifest m = get_manifest();
    m.apk_file = upload->filename;
    if (req.has_param("program_version")) {
      try {
        m.program_version = std::stoi(req.get_param_value("program_version"));
      } catch (...) {
        res.status = 400;
        res.set_content(R"({"error":"bad program_version"})", "application/json; charset=utf-8");
        return;
      }
    } else {
      m.program_version += 1;
    }
    save_manifest(m);
    res.set_content(
        json{{"ok", true},
             {"apk_file", upload->filename},
             {"apk_size", upload->size},
             {"program_version", m.program_version}}
            .dump(),
        "application/json; charset=utf-8");
  });

  server.Post("/admin/api/update/integrity-md5", [](const httplib::Request& req, httplib::Response& res) {
    json body;
    try {
      body = json::parse(req.body.empty() ? "{}" : req.body);
    } catch (...) {
      res.status = 400;
      res.set_content(R"({"error":"invalid json"})", "application/json; charset=utf-8");
      return;
    }

    std::string md5 = body.value("apk_md5", "");
    for (char& c : md5) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (md5.size() != 32 ||
        !std::all_of(md5.begin(), md5.end(), [](unsigned char c) { return std::isxdigit(c) != 0; })) {
      res.status = 400;
      res.set_content(R"({"error":"apk_md5 must be 32 hex chars"})",
                      "application/json; charset=utf-8");
      return;
    }

    uint64_t apk_size = 0;
    if (body.contains("apk_size")) {
      try {
        if (body["apk_size"].is_number_unsigned() || body["apk_size"].is_number_integer()) {
          if (body["apk_size"].get<int64_t>() < 0) {
            throw std::runtime_error("negative");
          }
          apk_size = body["apk_size"].get<uint64_t>();
        } else if (body["apk_size"].is_string()) {
          apk_size = static_cast<uint64_t>(std::stoull(body["apk_size"].get<std::string>()));
        } else {
          throw std::runtime_error("bad type");
        }
      } catch (...) {
        res.status = 400;
        res.set_content(R"({"error":"invalid apk_size"})", "application/json; charset=utf-8");
        return;
      }
    }

    std::string filename = body.value("filename", "local.apk");
    if (filename.empty() || filename.find('/') != std::string::npos ||
        filename.find('\\') != std::string::npos || filename.find("..") != std::string::npos) {
      filename = "local.apk";
    }
    if (filename.size() > 128) {
      filename = filename.substr(0, 128);
    }

    Manifest m = get_manifest();
    m.apk_md5 = md5;
    m.apk_size = apk_size;
    m.integrity_apk_file = filename;
    if (!save_manifest(m)) {
      res.status = 500;
      res.set_content(R"({"error":"manifest save failed"})", "application/json; charset=utf-8");
      return;
    }
    res.set_content(
        json{{"ok", true},
             {"integrity_apk_file", m.integrity_apk_file},
             {"apk_md5", m.apk_md5},
             {"apk_size", m.apk_size}}
            .dump(),
        "application/json; charset=utf-8");
  });

  server.Post("/admin/api/update/doc", [](const httplib::Request& req, httplib::Response& res) {
    std::string encrypted;
    if (!normalize_doc_content(req.body, encrypted)) {
      res.status = 400;
      res.set_content(R"({"error":"invalid doc.json or file exceeds 32MB"})",
                      "application/json; charset=utf-8");
      return;
    }

    Manifest m = get_manifest();
    {
      std::lock_guard<std::mutex> lock(g_mu);
      if (!write_doc_unlocked(encrypted) || !refresh_doc_metadata_unlocked(m)) {
        res.status = 500;
        res.set_content(R"({"error":"save failed"})", "application/json; charset=utf-8");
        return;
      }
    }
    m.doc_version += 1;
    if (!save_manifest(m)) {
      res.status = 500;
      res.set_content(R"({"error":"manifest save failed"})", "application/json; charset=utf-8");
      return;
    }
    res.set_content(
        json{{"ok", true},
             {"doc_version", m.doc_version},
             {"doc_md5", m.doc_md5},
             {"doc_size", m.doc_size}}
            .dump(),
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
