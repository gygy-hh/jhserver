#include "jh/handlers.hpp"

#include "jh/auth.hpp"
#include "jh/cache.hpp"
#include "jh/crypto.hpp"
#include "jh/mail.hpp"
#include "jh/save_blob.hpp"
#include "jh/session.hpp"
#include "jh/storage.hpp"
#include "jh/update.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>

namespace jh::handlers {

namespace {

using json = nlohmann::json;

TtlCache<std::string, json> g_init_data_cache(512);
constexpr auto kInitDataCacheTtl = std::chrono::seconds(30);
constexpr int kMaxSaveArea = 66;

template <typename Loader>
json cached_init_data(const std::string& key, Loader&& loader) {
  if (const auto cached = g_init_data_cache.get(key)) {
    return *cached;
  }
  json value = loader();
  g_init_data_cache.put(key, value, kInitDataCacheTtl);
  return value;
}

std::string mail_begin_at(const std::optional<json>& activities) {
  if (!activities || !activities->is_array()) {
    return {};
  }
  for (const auto& activity : *activities) {
    if (activity.is_object() && activity.value("type", "") == "s_goldSum") {
      return activity.value("beginAt", "");
    }
  }
  return {};
}

struct RequestCtx {
  const ServerConfig* config;
  std::string action;
  int ver = 0;
  std::string channel;
};

std::string json_compact(const json& j) {
  return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string make_error(int code, const std::string& msg) {
  return json_compact(json{{"code", code}, {"msg", msg}});
}

bool parse_query(const httplib::Request& req, RequestCtx& ctx) {
  ctx.ver = ctx.config->game_version;
  if (req.has_param("ver")) {
    try {
      ctx.ver = std::stoi(req.get_param_value("ver"));
    } catch (...) {
      return false;
    }
  }
  ctx.channel = req.has_param("channel") ? req.get_param_value("channel") : "none";
  if (req.has_param("plat")) {
    const auto plat = req.get_param_value("plat");
    if (!plat.empty() && plat != ctx.config->plat) {
      // 客户端固定 ANDR，非致命
    }
  }
  return true;
}

std::string extract_action(const httplib::Request& req) {
  std::string path = req.path;
  while (!path.empty() && path.front() == '/') {
    path.erase(path.begin());
  }
  const auto q = path.find('?');
  if (q != std::string::npos) {
    path = path.substr(0, q);
  }
  return path;
}

json parse_encrypted_body(const std::string& body, int ver) {
  if (body.empty()) {
    return json::object();
  }
  const std::string plain = crypto::decrypt_payload(body, ver);
  json j = json::parse(plain);
  if (!j.is_object()) {
    throw std::runtime_error("JSON must be object");
  }
  return j;
}

std::string trim_string(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n' || s.back() == '\t')) {
    s.pop_back();
  }
  size_t start = 0;
  while (start < s.size() && (s[start] == ' ' || s[start] == '\r' || s[start] == '\n' || s[start] == '\t')) {
    ++start;
  }
  return s.substr(start);
}

void skip_json_ws(const std::string& s, size_t& i) {
  while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) {
    ++i;
  }
}

bool json_locate_key(const std::string& s, const char* key, size_t& value_pos) {
  const std::string pat = std::string("\"") + key + "\"";
  size_t p = 0;
  while (true) {
    p = s.find(pat, p);
    if (p == std::string::npos) {
      return false;
    }
    size_t j = p + pat.size();
    skip_json_ws(s, j);
    if (j < s.size() && s[j] == ':') {
      value_pos = j + 1;
      skip_json_ws(s, value_pos);
      return true;
    }
    p += pat.size();
  }
}

std::string json_parse_string_value(const std::string& s, size_t& i) {
  std::string out;
  if (i >= s.size() || s[i] != '"') {
    return out;
  }
  ++i;
  out.reserve(s.size() > i ? s.size() - i : 0);
  while (i < s.size()) {
    const char c = s[i++];
    if (c == '"') {
      break;
    }
    if (c == '\\' && i < s.size()) {
      const char e = s[i++];
      switch (e) {
        case 'n':
          out.push_back('\n');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case '"':
        case '\\':
        case '/':
          out.push_back(e);
          break;
        case 'u':
          if (i + 4 <= s.size()) {
            i += 4;
          }
          out.push_back('?');
          break;
        default:
          out.push_back(e);
          break;
      }
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::optional<std::string> json_get_string(const std::string& s, const char* key) {
  size_t i = 0;
  if (!json_locate_key(s, key, i)) {
    return std::nullopt;
  }
  if (i < s.size() && s[i] == '"') {
    return json_parse_string_value(s, i);
  }
  if (i + 4 <= s.size() && s.compare(i, 4, "null") == 0) {
    return std::string();
  }
  return std::nullopt;
}

std::optional<int> json_get_int(const std::string& s, const char* key) {
  size_t i = 0;
  if (!json_locate_key(s, key, i)) {
    return std::nullopt;
  }
  if (i < s.size() && s[i] == '"') {
    try {
      return std::stoi(json_parse_string_value(s, i));
    } catch (...) {
      return std::nullopt;
    }
  }
  size_t j = i;
  if (j < s.size() && (s[j] == '-' || s[j] == '+')) {
    ++j;
  }
  while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])) != 0) {
    ++j;
  }
  if (j == i) {
    return std::nullopt;
  }
  try {
    return std::stoi(s.substr(i, j - i));
  } catch (...) {
    return std::nullopt;
  }
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

std::string url_decode(const std::string& input) {
  std::string out;
  out.reserve(input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '+') {
      out.push_back(' ');
    } else if (input[i] == '%' && i + 2 < input.size()) {
      const int hi = hex_value(input[i + 1]);
      const int lo = hex_value(input[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
      out.push_back(input[i]);
    } else {
      out.push_back(input[i]);
    }
  }
  return out;
}

std::string normalize_phone(std::string raw) {
  raw = trim_string(url_decode(raw));
  if (raw.rfind("+86", 0) == 0) {
    raw = raw.substr(3);
  } else if (raw.rfind("86", 0) == 0 && raw.size() > 11) {
    raw = raw.substr(2);
  }
  raw = trim_string(raw);

  std::string digits;
  digits.reserve(raw.size());
  for (unsigned char c : raw) {
    if (std::isdigit(c) != 0) {
      digits.push_back(static_cast<char>(c));
    }
  }
  return digits;
}

bool is_valid_cn_mobile(const std::string& phone) {
  if (phone.size() != 11) {
    return false;
  }
  if (phone[0] != '1') {
    return false;
  }
  for (char c : phone) {
    if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
      return false;
    }
  }
  return true;
}

json parse_sms_form(const std::string& body) {
  json j = json::object();
  const std::string trimmed = trim_string(body);
  if (trimmed.empty()) {
    return j;
  }

  std::istringstream iss(trimmed);
  std::string pair;
  while (std::getline(iss, pair, '&')) {
    pair = trim_string(pair);
    const auto eq = pair.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    const std::string key = trim_string(pair.substr(0, eq));
    const std::string val = trim_string(url_decode(pair.substr(eq + 1)));
    j[key] = val;
  }

  if (j.empty() && trimmed.find('=') == std::string::npos) {
    j["acc"] = trim_string(url_decode(trimmed));
  }
  return j;
}

std::string extract_sms_acc(const httplib::Request& req, const json& body_json) {
  if (body_json.contains("acc")) {
    return body_json.value("acc", "");
  }
  if (req.has_param("acc")) {
    return req.get_param_value("acc");
  }
  return trim_string(req.body);
}

std::string respond(const RequestCtx& ctx, const json& payload) {
  const std::string text = json_compact(payload);
  if (crypto::is_encrypted_response_action(ctx.action)) {
    return crypto::encrypt_payload(text, ctx.ver);
  }
  return text;
}

json api_ok(const json& extra = json::object()) {
  json resp{{"code", 0}, {"msg", ""}};
  for (auto it = extra.begin(); it != extra.end(); ++it) {
    resp[it.key()] = it.value();
  }
  return resp;
}

constexpr int64_t kManualSaveIntervalSec = 7LL * 24 * 60 * 60;

int64_t manual_remain_sec(int64_t last_at, int64_t now) {
  if (last_at <= 0) {
    return 0;
  }
  const int64_t left = last_at + kManualSaveIntervalSec - now;
  return left > 0 ? left : 0;
}

std::string manual_remain_msg(const char* action, int64_t remain_sec) {
  const int64_t days = (remain_sec + 24 * 60 * 60 - 1) / (24 * 60 * 60);
  return std::string(action) + "\u9700\u95f4\u96947\u5929\uff0c\u5269\u4f59\u7ea6" + std::to_string(days) + "\u5929";
}

json api_err(const std::string& msg, int code = 1) {
  return json{{"code", code}, {"msg", msg}};
}

std::optional<int64_t> request_repair_version(const json& req) {
  static const char* keys[] = {"repairVer", "repair_ver", "repair"};
  for (const char* key : keys) {
    const auto it = req.find(key);
    if (it != req.end() && it->is_number_integer()) {
      return it->get<int64_t>();
    }
  }
  return std::nullopt;
}

std::string enforce_bl_policy(const std::string& blob, const std::string& acc, int save_index, const char* where) {
  const std::string updated = save_blob::strip_bl(blob, save_index);
  if (updated.size() != blob.size()) {
    std::cerr << "[" << where << "] stripped bl acc=" << acc << " area=" << save_index << std::endl;
  }
  return updated;
}

bool requires_session(const std::string& action) {
  return action == "getInitData" || action == "changePassword";
}

bool bind_session_identity(json& req, std::string& error) {
  const auto acc = session_validate(req.value("session", ""));
  if (!acc) {
    error = "session required";
    return false;
  }
  const std::string requested_acc = req.value("acc", "");
  if (!requested_acc.empty() && requested_acc != *acc) {
    error = "session account mismatch";
    return false;
  }
  req["acc"] = *acc;
  return true;
}

json handle_find_save(const json& req) {
  const std::string acc = req.value("acc", "");
  int area = req.value("area", 0);
  if (area <= 0) {
    area = 1;
  }
  if (area > kMaxSaveArea) {
    return api_err("invalid area");
  }
  if (acc.empty()) {
    return api_err("acc required");
  }

  json data;
  if (auto meta = storage::get_save_meta(acc, area)) {
    data["username"] = meta->username;
    data["lev"] = meta->lev;
    data["saveTime"] = std::to_string(meta->save_time);
  } else {
    data["username"] = req.value("username", req.value("pName", acc));
    data["lev"] = req.value("lev", 1);
    data["saveTime"] = "0";
  }
  data["tRemain"] = 0;
  return json{{"code", 0}, {"data", data}};
}

json handle_get_init_data(const json& req, int /*ver*/, const std::string& channel) {
  const std::string acc = req.value("acc", "");
  if (acc.empty()) {
    return json{{"code", 1}, {"msg", "acc required"}};
  }
  int area = req.value("area", 0);
  if (area <= 0) {
    area = 1;
  }
  if (area > kMaxSaveArea) {
    return api_err("invalid area");
  }
  std::cerr << "[getInitData] channel=" << channel << " acc=" << acc << " area=" << area
            << std::endl;
  const int prog_ver = req.value("prog_ver", req.value("program_version", 0));
  const int json_ver = req.value("json_ver", 0);
  const int asset_ver = req.value("asset_ver", 0);
  auto account = storage::ensure_account(acc);
  const std::time_t now = std::time(nullptr);
  bool md5_suppressed = false;
  std::string client_apk_md5 = req.value("apk_md5", "");
  std::string server_apk_md5 = update::get_manifest().apk_md5;
  std::transform(client_apk_md5.begin(), client_apk_md5.end(), client_apk_md5.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  std::transform(server_apk_md5.begin(), server_apk_md5.end(), server_apk_md5.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (!storage::is_bl_exempt(acc) && server_apk_md5.size() == 32 &&
      !client_apk_md5.empty()) {
    md5_suppressed = client_apk_md5 != server_apk_md5;
    if (md5_suppressed) {
      std::cerr << "[getInitData] apk md5 mismatch, bl=1 acc=" << acc
                << " client=" << client_apk_md5 << " server=" << server_apk_md5 << std::endl;
    }
  }
  json repair = cached_init_data("repair", [] { return update::get_repair(); });
  if (!repair.is_array()) {
    repair = json::array();
  }

  auto personal_huo_dong = mail::take_huo_dong(channel, acc, area);
  auto global_huo_dong = mail::take_global_huo_dong(channel, acc, area);
  // 客户端只可靠处理一个 s_goldSum。与参考项目的覆盖语义一致，
  // 个人邮件和全体邮件同时存在时仅下发发送时间较新的一封。
  if (personal_huo_dong && global_huo_dong) {
    if (mail_begin_at(global_huo_dong) > mail_begin_at(personal_huo_dong)) {
      personal_huo_dong.reset();
    } else {
      global_huo_dong.reset();
    }
  }
  int64_t repair_version = 0;
  const json content_version = cached_init_data(
      "contentVersion", [] { return json(update::get_manifest().updated_at); });
  if (content_version.is_number_integer()) {
    repair_version = std::max(repair_version, content_version.get<int64_t>());
  }
  repair_version = std::max<int64_t>(repair_version, 1);
  for (const auto& item : repair) {
    repair_version =
        std::max(repair_version, item.value("repairVer", static_cast<int64_t>(0)));
  }
  if (const auto client_version = request_repair_version(req);
      client_version && *client_version == repair_version && !personal_huo_dong &&
      !global_huo_dong) {
    json unchanged{{"code", 0}, {"repairVer", repair_version}};
    if (md5_suppressed) {
      unchanged["bl"] = 1;
    }
    return unchanged;
  }

  bool has_current_version = false;
  for (const auto& item : repair) {
    if (item.value("repairVer", static_cast<int64_t>(0)) == repair_version) {
      has_current_version = true;
      break;
    }
  }
  if (!has_current_version) {
    repair.push_back(
        json{{"repairVer", repair_version}, {"dataJs", R"({"getProp":{}})"}});
  }
  std::stable_sort(repair.begin(), repair.end(), [](const json& a, const json& b) {
    return a.value("repairVer", static_cast<int64_t>(0)) <
           b.value("repairVer", static_cast<int64_t>(0));
  });

  json huo_dong = cached_init_data("huoDong", [] { return update::get_huo_dong(); });
  if (!huo_dong.is_array()) {
    huo_dong = json::array();
  }
  if (personal_huo_dong) {
    huo_dong = *personal_huo_dong;
    std::cerr << "[mailPush] channel=" << channel << " acc=" << acc << " area=" << area
              << " scope=personal protocol=redis_s_goldSum" << std::endl;
  } else if (global_huo_dong) {
    huo_dong = *global_huo_dong;
    std::cerr << "[mailPush] channel=" << channel << " acc=" << acc << " area=" << area
              << " scope=global protocol=redis_s_goldSum" << std::endl;
  }

  json resp{
      {"code", 0},
      {"repairVer", repair_version},
      {"tt", static_cast<int64_t>(now)},
      {"mtt", crypto::calc_mtt(account.id)},
      {"save_syn", req.value("save_syn", 0)},
      {"fight_syn", req.value("fight_syn", 0)},
      {"huoDong", std::move(huo_dong)},
      {"repair", repair},
      {"update",
       cached_init_data("update:" + std::to_string(prog_ver) + ":" + std::to_string(json_ver) + ":" +
                            std::to_string(asset_ver),
                        [=] { return update::get_client_update(prog_ver, json_ver, asset_ver); })},
      {"area", area},
  };
  if (md5_suppressed) {
    resp["bl"] = 1;
  }
  return resp;
}

json handle_login(const json& req) {
  const std::string acc = req.value("acc", "");
  const std::string psw = req.value("psw", "");
  const AuthOutcome auth =
      acc.find('@') != std::string::npos ? auth_login_mail(acc, psw) : auth_login_phone(acc, psw);
  if (auth.result != AuthResult::Ok) {
    return json{{"code", 1}, {"msg", auth.message}};
  }
  const auto session = session_issue(acc);
  if (!session) {
    return json{{"code", 1}, {"msg", "session create failed"}};
  }
  return json{{"code", 0},
              {"save_syn", 0},
              {"fight_syn", 0},
              {"acc", acc},
              {"is_new_account", auth.is_new_account},
              {"session", session->token},
              {"session_exp", session->expires_at}};
}

json handle_change_password(const json& req) {
  const std::string acc = req.value("acc", "");
  const std::string old_psw = req.value("old_psw", "");
  const std::string new_psw = req.value("new_psw", "");
  if (acc.empty()) {
    return api_err("session required", 401);
  }
  if (old_psw.empty()) {
    return api_err("old password required");
  }
  if (new_psw.empty()) {
    return api_err("new password required");
  }
  std::string error;
  if (!auth_change_password(acc, old_psw, new_psw, error)) {
    return api_err(error);
  }
  const auto session = session_issue(acc);
  if (!session) {
    return api_err("session create failed");
  }
  return api_ok(json{{"msg", "password changed"},
                     {"session", session->token},
                     {"session_exp", session->expires_at}});
}

json handle_sms_code(const json& req) {
  const std::string phone = normalize_phone(req.value("acc", ""));
  const AuthOutcome auth = auth_send_sms_code(phone);
  if (auth.result != AuthResult::Ok) {
    return json{{"code", 1}, {"msg", auth.message}};
  }
  return json{{"code", 0}, {"msg", ""}};
}

json handle_upload_save_fields(const std::string& acc, int area, const std::string& save_str,
                               const std::string& username, int lev) {
  if (acc.empty()) {
    return api_err("acc required");
  }
  if (save_str.empty()) {
    return api_err("save required");
  }
  int resolved_area = area;
  if (resolved_area <= 0) {
    resolved_area = 1;
  }

  const int64_t now = std::time(nullptr);
  storage::SaveMeta meta;
  meta.username = username.empty() ? acc : username;
  meta.lev = lev > 0 ? lev : 1;
  meta.save_time = now;
  meta.manual_upload_at = now;
  if (auto existing = storage::get_save_meta(acc, resolved_area)) {
    meta.manual_download_at = existing->manual_download_at;
  }

  std::cerr << "[uploadSave] acc=" << acc << " area=" << resolved_area << " bytes=" << save_str.size() << std::endl
            << std::flush;

  std::string cleaned = save_str;
  try {
    cleaned = enforce_bl_policy(cleaned, acc, resolved_area, "uploadSave");
  } catch (const std::exception& ex) {
    cleaned = save_str;
    std::cerr << "[uploadSave] bl policy skipped: " << ex.what() << std::endl;
  }

  if (!storage::save_cloud(acc, resolved_area, cleaned, meta)) {
    return api_err("save failed");
  }
  return api_ok(json{{"msg", "上传成功"}});
}

json handle_upload_save_plain(const std::string& plain, const std::string& authenticated_acc) {
  const std::string acc = authenticated_acc;
  const int area = json_get_int(plain, "area").value_or(1);
  const std::string save_str = json_get_string(plain, "save").value_or("");
  std::string username = json_get_string(plain, "username").value_or("");
  if (username.empty()) {
    username = json_get_string(plain, "pName").value_or("");
  }
  const int lev = json_get_int(plain, "lev").value_or(1);
  return handle_upload_save_fields(acc, area, save_str, username, lev);
}

json handle_upload_save(const json& req) {
  std::string save_str;
  if (req.contains("save")) {
    if (req["save"].is_string()) {
      save_str = req["save"].get<std::string>();
    } else {
      save_str = req["save"].dump(-1, ' ', false, json::error_handler_t::replace);
    }
  }
  return handle_upload_save_fields(req.value("acc", ""), req.value("area", 0), save_str,
                                   req.value("username", req.value("pName", "")), req.value("lev", 1));
}

json handle_download_save(const json& req) {
  const std::string acc = req.value("acc", "");
  int area = req.value("area", 0);
  if (area <= 0) {
    area = 1;
  }
  if (acc.empty()) {
    return api_err("acc required");
  }
  const int64_t now = std::time(nullptr);
  if (acc != "19848015669") {
    if (auto meta = storage::get_save_meta(acc, area)) {
      const int64_t remain = manual_remain_sec(meta->manual_download_at, now);
      if (remain > 0) {
        return api_err(manual_remain_msg("\u624b\u52a8\u4e0b\u8f7d", remain));
      }
    }
  }
  auto save = storage::load_cloud(acc, area);
  if (!save) {
    std::cerr << "[downloadSave] no save acc=" << acc << " area=" << area << std::endl;
    return json{{"code", 0}, {"msg", ""}, {"data", json{{"save", ""}}}, {"save", ""}};
  }

  try {
    *save = enforce_bl_policy(*save, acc, area, "downloadSave");
  } catch (const std::exception& ex) {
    std::cerr << "[downloadSave] bl policy skipped: " << ex.what() << std::endl;
  }

  storage::mark_manual_download(acc, area, now);
  std::cerr << "[downloadSave] acc=" << acc << " area=" << area << " bytes=" << save->size() << std::endl;
  return json{{"code", 0}, {"msg", ""}, {"data", json{{"save", *save}}}, {"save", *save}};
}

json handle_mail(const json& req) {
  const std::string acc = req.value("acc", "");
  const std::string psw = req.value("psw", "");
  const AuthOutcome auth = auth_login_mail(acc, psw);
  if (auth.result != AuthResult::Ok) {
    return json{{"code", 1}, {"msg", auth.message}};
  }
  const auto session = session_issue(acc);
  if (!session) {
    return json{{"code", 1}, {"msg", "session create failed"}};
  }
  return json{{"code", 0},
              {"save_syn", 0},
              {"fight_syn", 0},
              {"acc", acc},
              {"is_new_account", auth.is_new_account},
              {"session", session->token},
              {"session_exp", session->expires_at}};
}

json handle_stub(const json& /*req*/) {
  return json{{"code", 0}, {"msg", "ok"}};
}

json dispatch(const RequestCtx& ctx, const json& req) {
  if (ctx.action == "getInitData") {
    return handle_get_init_data(req, ctx.ver, ctx.channel);
  }
  if (ctx.action == "login") {
    return handle_login(req);
  }
  if (ctx.action == "register" || ctx.action == "regist") {
    return handle_login(req);
  }
  if (ctx.action == "changePassword") {
    return handle_change_password(req);
  }
  if (ctx.action == "smsCode") {
    return handle_sms_code(req);
  }
  if (ctx.action == "uploadSave") {
    return handle_upload_save(req);
  }
  if (ctx.action == "findSave") {
    return handle_find_save(req);
  }
  if (ctx.action == "downloadSave") {
    return handle_download_save(req);
  }
  if (ctx.action == "mail") {
    return handle_mail(req);
  }
  return handle_stub(req);
}

void handle_post(const httplib::Request& req, httplib::Response& res, const ServerConfig& config) {
  RequestCtx ctx{&config, extract_action(req)};
  res.status = 200;
  std::cerr << "[http] " << req.method << " " << req.path << " body=" << req.body.size() << std::endl << std::flush;
  if (!parse_query(req, ctx)) {
    res.set_content(make_error(1, "bad ver"), "application/json; charset=utf-8");
    return;
  }
  if (ctx.action.empty()) {
    res.set_content(make_error(1, "unknown action"), "application/json; charset=utf-8");
    return;
  }

  try {
    json out;
    if (ctx.action == "smsCode") {
      json body_json = parse_sms_form(req.body);
      const std::string raw_acc = extract_sms_acc(req, body_json);
      if (!raw_acc.empty()) {
        body_json["acc"] = raw_acc;
      }
      out = dispatch(ctx, body_json);
    } else if (ctx.action == "uploadSave") {
      const std::string plain = crypto::decrypt_payload(trim_string(req.body), ctx.ver);
      std::cerr << "[uploadSave] decrypted=" << plain.size() << std::endl << std::flush;
      const std::string acc = json_get_string(plain, "acc").value_or("");
      out = handle_upload_save_plain(plain, acc);
    } else if (crypto::is_encrypted_body_action(ctx.action)) {
      json body = parse_encrypted_body(trim_string(req.body), ctx.ver);
      std::string session_error;
      if (requires_session(ctx.action) && !bind_session_identity(body, session_error)) {
        std::cerr << "[session] rejected action=" << ctx.action << " channel=" << ctx.channel
                  << " reason=" << session_error
                  << std::endl;
        out = api_err(session_error, 401);
      } else {
        out = dispatch(ctx, body);
      }
    } else {
      out = dispatch(ctx, req.body.empty() ? json::object() : json::parse(req.body));
    }
    res.set_content(respond(ctx, out), "application/json; charset=utf-8");
  } catch (const std::exception& ex) {
    std::cerr << "[error] " << ctx.action << ": " << ex.what() << std::endl << std::flush;
    if (crypto::is_encrypted_response_action(ctx.action)) {
      try {
        res.set_content(crypto::encrypt_payload(make_error(1, ex.what()), ctx.ver), "text/plain; charset=utf-8");
        return;
      } catch (...) {
      }
    }
    res.set_content(make_error(1, ex.what()), "application/json; charset=utf-8");
  }
}

}  // namespace

void register_routes(httplib::Server& server, const ServerConfig& config) {
  server.Post(R"(/.+)", [&config](const httplib::Request& req, httplib::Response& res) {
    if (req.path.rfind("/admin", 0) == 0) {
      res.status = 404;
      res.set_content(R"({"code":1,"msg":"not found"})", "application/json; charset=utf-8");
      return;
    }
    handle_post(req, res, config);
  });

  server.Get("/health", [](const httplib::Request&, httplib::Response& res) {
    res.set_content(R"({"status":"ok"})", "application/json");
  });

}

}  // namespace jh::handlers
