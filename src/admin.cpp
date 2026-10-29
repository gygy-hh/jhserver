#include "jh/admin.hpp"

#include "jh/auth.hpp"
#include "jh/gateway.hpp"
#include "jh/config.hpp"

#include "jh/crypto.hpp"
#include "jh/mail.hpp"
#include "jh/push_hub.hpp"
#include "jh/save_blob.hpp"
#include "jh/save_crypto.hpp"
#include "jh/storage.hpp"
#include "jh/update.hpp"

#include <ctime>
#include <exception>
#include <fstream>
#include <iomanip>
#include <map>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

namespace jh::admin {

namespace {

using json = nlohmann::json;

std::string load_admin_html() {
  const std::string path = find_resource_path("web/admin.html");
  std::ifstream in(path, std::ios::binary);
  if (in) {
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }
  return "<html><body><h1>admin.html not found</h1><p>请确认 web/admin.html 存在</p><p>查找路径: " + path +
         "</p></body></html>";
}

std::string format_time(int64_t ts) {
  if (ts <= 0) {
    return "-";
  }
  std::time_t t = static_cast<std::time_t>(ts);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
  return oss.str();
}

std::string format_size(size_t bytes) {
  if (bytes < 1024) {
    return std::to_string(bytes) + " B";
  }
  if (bytes < 1024 * 1024) {
    return std::to_string(bytes / 1024) + " KB";
  }
  return std::to_string(bytes / (1024 * 1024)) + " MB";
}

// 管理端查看存档时，把 dat.json 解密成缩进后的 JSON。密钥与区服编号一致。
std::string preview_dat_plain(const std::string& content, int area) {
  if (content.empty() || content == "null") {
    return content;
  }
  std::string plain = content;
  const char head = content.front();
  if (head != '{' && head != '[') {
    try {
      plain = save_crypto::decrypt_dat_auto(content, area, nullptr);
    } catch (const std::exception& ex) {
      return std::string("[dat.json 解密失败: ") + ex.what() + "]\n" + content;
    }
  }
  try {
    return nlohmann::ordered_json::parse(plain).dump(2);
  } catch (...) {
    return plain;
  }
}

std::string preview_cloud_save(const std::string& blob, int area, bool* truncated) {
  constexpr size_t kMaxPreview = 512 * 1024;
  const auto segments = save_blob::parse(blob);
  std::ostringstream oss;
  auto write_segment = [&](const save_blob::SaveSegment& seg) {
    if (oss.tellp() > 0) {
      oss << "\n\n";
    }
    oss << seg.name << "\n";
    if (seg.name == "dat.json") {
      oss << preview_dat_plain(seg.content, area);
    } else {
      oss << seg.content;
    }
  };

  bool has_dat = false;
  for (const auto& seg : segments) {
    if (seg.name == "dat.json") {
      write_segment(seg);
      has_dat = true;
    }
  }
  for (const auto& seg : segments) {
    if (seg.name != "dat.json") {
      write_segment(seg);
    }
  }
  if (!has_dat && segments.empty()) {
    oss << blob;
  }

  std::string preview = oss.str();
  const bool cut = preview.size() > kMaxPreview;
  if (cut) {
    preview.resize(kMaxPreview);
    preview += "\n\n... [truncated] ...";
  }
  if (truncated) {
    *truncated = cut;
  }
  return preview;
}

}  // namespace

void register_routes(gateway::Router& server, const ServerConfig& config) {
  server.Get("/admin", [](const httplib::Request&, httplib::Response& res) {
    res.set_content(load_admin_html(), "text/html; charset=utf-8");
  });

  server.Get("/admin/", [](const httplib::Request&, httplib::Response& res) {
    res.set_redirect("/admin");
  });

  server.Get("/admin/api/stats", [&config](const httplib::Request&, httplib::Response& res) {
    const auto stats = storage::get_stats();
    json j{
        {"account_count", stats.account_count},
        {"save_count", stats.save_count},
        {"next_id", stats.next_id},
        {"host", config.host},
        {"port", config.port},
        {"push_port", config.push_port},
        {"push_online", push_hub::online_count()},
        {"game_version", config.game_version},
        {"plat", config.plat},
        {"data_dir", config.data_dir},
        {"crypto_key", crypto::get_zhiling_psw(config.game_version)},
        {"mysql_host", config.mysql.host},
        {"mysql_database", config.mysql.database},
        {"admin_acc", config.admin_acc},
        {"min_password_len", config.min_password_len},
        {"server_time", format_time(std::time(nullptr))},
    };
    res.set_content(j.dump(2), "application/json; charset=utf-8");
  });

  server.Get("/admin/api/accounts", [](const httplib::Request&, httplib::Response& res) {
    json items = json::array();
    for (const auto& acc : storage::list_accounts_auth()) {
      items.push_back({{"acc", acc.acc},
                       {"id", acc.id},
                       {"mtt", crypto::calc_mtt(acc.id)},
                       {"has_password", acc.has_password},
                       {"bl_exempt", acc.bl_exempt},
                       {"created_at", acc.created_at},
                       {"created_at_text", format_time(acc.created_at)}});
    }
    res.set_content(json{{"items", items}}.dump(), "application/json; charset=utf-8");
  });

  server.Get("/admin/api/saves", [](const httplib::Request&, httplib::Response& res) {
    json items = json::array();
    for (const auto& save : storage::list_saves()) {
      items.push_back({{"acc", save.acc},
                       {"username", save.username},
                       {"area", save.area},
                       {"lev", save.lev},
                       {"size", save.size},
                       {"size_text", format_size(save.size)},
                       {"has_blob", save.has_blob},
                       {"has_meta", save.has_meta},
                       {"updated_at", save.updated_at},
                       {"updated_at_text", format_time(save.updated_at)}});
    }
    res.set_content(json{{"items", items}}.dump(), "application/json; charset=utf-8");
  });

  server.Get("/admin/api/save", [](const httplib::Request& req, httplib::Response& res) {
    if (!req.has_param("acc")) {
      res.status = 400;
      res.set_content(R"({"error":"acc required"})", "application/json; charset=utf-8");
      return;
    }
    const std::string acc = req.get_param_value("acc");
    int area = 0;
    if (req.has_param("area")) {
      area = std::stoi(req.get_param_value("area"));
    }
    auto save = storage::load_cloud(acc, area);
    if (!save) {
      res.status = 404;
      res.set_content(R"({"error":"not found"})", "application/json; charset=utf-8");
      return;
    }
    bool truncated = false;
    const std::string preview = preview_cloud_save(*save, area, &truncated);
    json j{{"acc", acc}, {"area", area}, {"size", save->size()}, {"preview", preview}, {"truncated", truncated}};
    res.set_content(j.dump(-1, ' ', false, json::error_handler_t::replace), "application/json; charset=utf-8");
  });

  server.Delete("/admin/api/save", [](const httplib::Request& req, httplib::Response& res) {
    if (!req.has_param("acc")) {
      res.status = 400;
      res.set_content(R"({"error":"acc required"})", "application/json; charset=utf-8");
      return;
    }
    const std::string acc = req.get_param_value("acc");
    int area = 0;
    if (req.has_param("area")) {
      area = std::stoi(req.get_param_value("area"));
    }
    if (!storage::delete_save(acc, area)) {
      res.status = 404;
      res.set_content(R"({"error":"not found"})", "application/json; charset=utf-8");
      return;
    }
    res.set_content(R"({"ok":true})", "application/json; charset=utf-8");
  });

  server.Post("/admin/api/save/download-cd/reset",
              [](const httplib::Request& req, httplib::Response& res) {
    json body;
    try {
      body = req.body.empty() ? json::object() : json::parse(req.body);
    } catch (...) {
      res.status = 400;
      res.set_content(R"({"error":"invalid json"})", "application/json; charset=utf-8");
      return;
    }
    const std::string acc = body.value("acc", "");
    const int area = body.value("area", 0);
    if (acc.empty() || area <= 0) {
      res.status = 400;
      res.set_content(R"({"error":"valid acc and area required"})",
                      "application/json; charset=utf-8");
      return;
    }
    if (!storage::clear_manual_download(acc, area)) {
      res.status = 404;
      res.set_content(R"({"error":"save metadata not found"})",
                      "application/json; charset=utf-8");
      return;
    }
    res.set_content(R"({"ok":true})", "application/json; charset=utf-8");
  });

  server.Delete("/admin/api/account", [](const httplib::Request& req, httplib::Response& res) {
    if (!req.has_param("acc")) {
      res.status = 400;
      res.set_content(R"({"error":"acc required"})", "application/json; charset=utf-8");
      return;
    }
    const std::string acc = req.get_param_value("acc");
    if (!storage::delete_account(acc)) {
      res.status = 404;
      res.set_content(R"({"error":"not found"})", "application/json; charset=utf-8");
      return;
    }
    res.set_content(R"({"ok":true})", "application/json; charset=utf-8");
  });

  server.Post("/admin/api/account/password", [](const httplib::Request& req, httplib::Response& res) {
    json body;
    try {
      body = req.body.empty() ? json::object() : json::parse(req.body);
    } catch (...) {
      res.status = 400;
      res.set_content(R"({"error":"invalid json"})", "application/json; charset=utf-8");
      return;
    }
    const std::string acc = body.value("acc", "");
    const std::string psw = body.value("psw", body.value("password", "123456"));
    if (acc.empty()) {
      res.status = 400;
      res.set_content(R"({"error":"acc required"})", "application/json; charset=utf-8");
      return;
    }
    std::string error;
    if (!auth_set_password(acc, psw, error)) {
      res.status = 400;
      res.set_content(json{{"error", error}}.dump(), "application/json; charset=utf-8");
      return;
    }
    res.set_content(json{{"ok", true}, {"acc", acc}}.dump(), "application/json; charset=utf-8");
  });

  server.Post("/admin/api/account/bl-exempt", [](const httplib::Request& req, httplib::Response& res) {
    json body;
    try {
      body = req.body.empty() ? json::object() : json::parse(req.body);
    } catch (...) {
      res.status = 400;
      res.set_content(R"({"error":"invalid json"})", "application/json; charset=utf-8");
      return;
    }
    const std::string acc = body.value("acc", "");
    if (acc.empty() || !body.contains("exempt") || !body["exempt"].is_boolean()) {
      res.status = 400;
      res.set_content(R"({"error":"acc and exempt required"})", "application/json; charset=utf-8");
      return;
    }
    const bool exempt = body["exempt"].get<bool>();
    if (!storage::set_bl_exempt(acc, exempt)) {
      res.status = 404;
      res.set_content(R"({"error":"account not found"})", "application/json; charset=utf-8");
      return;
    }
    res.set_content(json{{"ok", true}, {"acc", acc}, {"bl_exempt", exempt}}.dump(),
                    "application/json; charset=utf-8");
  });

  // 强制镇压：登记账号 + 给其全部云存档写入 bl=1
  server.Post("/admin/api/account/bl-force", [](const httplib::Request& req, httplib::Response& res) {
    json body;
    try {
      body = req.body.empty() ? json::object() : json::parse(req.body);
    } catch (...) {
      res.status = 400;
      res.set_content(R"({"error":"invalid json"})", "application/json; charset=utf-8");
      return;
    }
    const std::string acc = body.value("acc", "");
    if (acc.empty() || !body.contains("forced") || !body["forced"].is_boolean()) {
      res.status = 400;
      res.set_content(R"({"error":"acc and forced required"})", "application/json; charset=utf-8");
      return;
    }
    const bool forced = body["forced"].get<bool>();
    if (!storage::set_bl_forced(acc, forced)) {
      res.status = 500;
      res.set_content(R"({"error":"failed to update bl_force"})", "application/json; charset=utf-8");
      return;
    }

    json areas = json::array();
    int applied = 0;
    int failed = 0;
    if (forced) {
      for (const auto& save : storage::list_saves()) {
        if (save.acc != acc || !save.has_blob) {
          continue;
        }
        auto blob = storage::load_cloud(acc, save.area);
        if (!blob) {
          failed++;
          areas.push_back({{"area", save.area}, {"ok", false}, {"error", "load failed"}});
          continue;
        }
        try {
          const std::string updated = save_blob::apply_bl(*blob, save.area);
          storage::SaveMeta meta;
          if (auto existing = storage::get_save_meta(acc, save.area)) {
            meta = *existing;
          } else {
            meta.username = save.username.empty() ? acc : save.username;
            meta.lev = save.lev;
            meta.save_time = save.updated_at;
          }
          if (!storage::save_cloud(acc, save.area, updated, meta)) {
            failed++;
            areas.push_back({{"area", save.area}, {"ok", false}, {"error", "save failed"}});
            continue;
          }
          applied++;
          areas.push_back({{"area", save.area}, {"ok", true}, {"changed", updated.size() != blob->size()}});
        } catch (const std::exception& ex) {
          failed++;
          areas.push_back({{"area", save.area}, {"ok", false}, {"error", ex.what()}});
        }
      }
    }

    res.set_content(json{{"ok", true},
                         {"acc", acc},
                         {"bl_forced", forced},
                         {"applied", applied},
                         {"failed", failed},
                         {"areas", areas}}
                        .dump(-1, ' ', false, json::error_handler_t::replace),
                    "application/json; charset=utf-8");
  });

  server.Get("/admin/api/mails", [](const httplib::Request& req, httplib::Response& res) {
    if (!req.has_param("channel")) {
      res.status = 400;
      res.set_content(R"({"error":"channel required"})", "application/json; charset=utf-8");
      return;
    }
    const std::string channel = req.get_param_value("channel");
    json items = json::array();
    for (const auto& m : mail::list(channel)) {
      json props = json::object();
      for (const auto& [k, v] : m.items) {
        props[k] = v;
      }
      items.push_back({{"channel", m.channel},
                       {"acc", m.acc},
                       {"area", m.area},
                       {"items", props},
                       {"begin_at", m.begin_at},
                       {"end_at", m.end_at}});
    }
    res.set_content(json{{"channel", channel}, {"items", items}}.dump(2),
                    "application/json; charset=utf-8");
  });

  server.Get("/admin/api/mail", [](const httplib::Request& req, httplib::Response& res) {
    if (!req.has_param("channel") || !req.has_param("acc") || !req.has_param("area")) {
      res.status = 400;
      res.set_content(R"({"error":"channel, acc and area required"})",
                      "application/json; charset=utf-8");
      return;
    }
    int area = 0;
    try {
      area = std::stoi(req.get_param_value("area"));
    } catch (...) {
    }
    const auto entry =
        mail::detail(req.get_param_value("channel"), req.get_param_value("acc"), area);
    if (!entry) {
      res.status = 404;
      res.set_content(R"({"error":"mail not found"})", "application/json; charset=utf-8");
      return;
    }
    json props = json::object();
    for (const auto& [prop_id, count] : entry->items) {
      props[prop_id] = count;
    }
    res.set_content(json{{"channel", entry->channel},
                         {"acc", entry->acc},
                         {"area", entry->area},
                         {"items", props},
                         {"begin_at", entry->begin_at},
                         {"end_at", entry->end_at}}
                        .dump(2),
                    "application/json; charset=utf-8");
  });

  server.Post("/admin/api/mail", [](const httplib::Request& req, httplib::Response& res) {
    json body;
    try {
      body = req.body.empty() ? json::object() : json::parse(req.body);
    } catch (...) {
      res.status = 400;
      res.set_content(R"({"error":"invalid json"})", "application/json; charset=utf-8");
      return;
    }
    const std::string channel = body.value("channel", "");
    const std::string acc = body.value("acc", "");
    std::map<std::string, int> items;
    if (body.contains("items") && body["items"].is_object()) {
      for (auto it = body["items"].begin(); it != body["items"].end(); ++it) {
        if (it.value().is_number_integer()) {
          const int count = it.value().get<int>();
          if (count > 0) {
            items[it.key()] = count;
          }
        }
      }
    } else if (body.contains("prop_id")) {
      const std::string prop_id = std::to_string(body.value("prop_id", 0));
      items[prop_id] = body.value("count", 1);
    }
    if (items.empty()) {
      res.status = 400;
      res.set_content(R"({"error":"items required"})", "application/json; charset=utf-8");
      return;
    }
    if (channel.empty() || channel.find(':') != std::string::npos || acc.empty() ||
        acc.find(':') != std::string::npos) {
      res.status = 400;
      res.set_content(R"({"error":"valid channel and acc required"})",
                      "application/json; charset=utf-8");
      return;
    }
    const int area = body.value("area", 0);
    if (area <= 0) {
      res.status = 400;
      res.set_content(R"({"error":"area required"})", "application/json; charset=utf-8");
      return;
    }
    storage::ensure_account(acc);
    if (!mail::send(channel, acc, area, update::get_huo_dong(), items)) {
      res.status = 503;
      res.set_content(R"({"error":"redis mail write failed"})",
                      "application/json; charset=utf-8");
      return;
    }
    res.set_content(
        json{{"ok", true}, {"channel", channel}, {"acc", acc}, {"area", area}}.dump(),
        "application/json; charset=utf-8");
  });

  server.Delete("/admin/api/mail", [](const httplib::Request& req, httplib::Response& res) {
    if (!req.has_param("channel") || !req.has_param("acc") || !req.has_param("area")) {
      res.status = 400;
      res.set_content(R"({"error":"channel, acc and area required"})",
                      "application/json; charset=utf-8");
      return;
    }
    const std::string channel = req.get_param_value("channel");
    const std::string acc = req.get_param_value("acc");
    int area = 0;
    try {
      area = std::stoi(req.get_param_value("area"));
    } catch (...) {
    }
    if (!mail::remove(channel, acc, area)) {
      res.status = 404;
      res.set_content(R"({"error":"not found"})", "application/json; charset=utf-8");
      return;
    }
    res.set_content(R"({"ok":true})", "application/json; charset=utf-8");
  });

  server.Get("/admin/api/global-mail", [](const httplib::Request& req, httplib::Response& res) {
    if (!req.has_param("channel")) {
      res.status = 400;
      res.set_content(R"({"error":"channel required"})", "application/json; charset=utf-8");
      return;
    }
    const std::string channel = req.get_param_value("channel");
    const auto entry = mail::global_detail(channel);
    if (!entry) {
      res.set_content(json{{"channel", channel}, {"active", false}}.dump(),
                      "application/json; charset=utf-8");
      return;
    }
    json props = json::object();
    for (const auto& [prop_id, count] : entry->items) {
      props[prop_id] = count;
    }
    res.set_content(json{{"channel", channel},
                         {"active", true},
                         {"items", props},
                         {"begin_at", entry->begin_at},
                         {"end_at", entry->end_at}}
                        .dump(2),
                    "application/json; charset=utf-8");
  });

  server.Post("/admin/api/global-mail", [](const httplib::Request& req, httplib::Response& res) {
    json body;
    try {
      body = req.body.empty() ? json::object() : json::parse(req.body);
    } catch (...) {
      res.status = 400;
      res.set_content(R"({"error":"invalid json"})", "application/json; charset=utf-8");
      return;
    }
    const std::string channel = body.value("channel", "");
    std::map<std::string, int> items;
    if (body.contains("items") && body["items"].is_object()) {
      for (auto it = body["items"].begin(); it != body["items"].end(); ++it) {
        if (it.value().is_number_integer()) {
          const int count = it.value().get<int>();
          if (count > 0) {
            items[it.key()] = count;
          }
        }
      }
    }
    if (channel.empty() || channel.find(':') != std::string::npos) {
      res.status = 400;
      res.set_content(R"({"error":"valid channel required"})",
                      "application/json; charset=utf-8");
      return;
    }
    if (items.empty()) {
      res.status = 400;
      res.set_content(R"({"error":"items required"})", "application/json; charset=utf-8");
      return;
    }
    if (!mail::send_global(channel, update::get_huo_dong(), items)) {
      res.status = 503;
      res.set_content(R"({"error":"redis global mail write failed"})",
                      "application/json; charset=utf-8");
      return;
    }
    res.set_content(json{{"ok", true}, {"channel", channel}, {"scope", "global"}}.dump(),
                    "application/json; charset=utf-8");
  });

  server.Delete("/admin/api/global-mail", [](const httplib::Request& req,
                                              httplib::Response& res) {
    if (!req.has_param("channel")) {
      res.status = 400;
      res.set_content(R"({"error":"channel required"})", "application/json; charset=utf-8");
      return;
    }
    if (!mail::remove_global(req.get_param_value("channel"))) {
      res.status = 404;
      res.set_content(R"({"error":"not found"})", "application/json; charset=utf-8");
      return;
    }
    res.set_content(R"({"ok":true})", "application/json; charset=utf-8");
  });

  server.Get("/admin/api/global-mail/schedules",
             [](const httplib::Request& req, httplib::Response& res) {
    if (!req.has_param("channel")) {
      res.status = 400;
      res.set_content(R"({"error":"channel required"})", "application/json; charset=utf-8");
      return;
    }
    json items = json::array();
    for (const auto& schedule : mail::list_global_schedules(req.get_param_value("channel"))) {
      json props = json::object();
      for (const auto& [prop_id, count] : schedule.items) {
        props[prop_id] = count;
      }
      items.push_back({{"id", schedule.id},
                       {"channel", schedule.channel},
                       {"items", props},
                       {"scheduled_at", schedule.scheduled_at},
                       {"scheduled_at_text", format_time(schedule.scheduled_at)},
                       {"created_at", schedule.created_at}});
    }
    res.set_content(json{{"items", items}}.dump(2), "application/json; charset=utf-8");
  });

  server.Post("/admin/api/global-mail/schedules",
              [](const httplib::Request& req, httplib::Response& res) {
    json body;
    try {
      body = req.body.empty() ? json::object() : json::parse(req.body);
    } catch (...) {
      res.status = 400;
      res.set_content(R"({"error":"invalid json"})", "application/json; charset=utf-8");
      return;
    }
    const std::string channel = body.value("channel", "");
    const int64_t scheduled_at = body.value("scheduled_at", static_cast<int64_t>(0));
    std::map<std::string, int> items;
    if (body.contains("items") && body["items"].is_object()) {
      for (auto it = body["items"].begin(); it != body["items"].end(); ++it) {
        if (it.value().is_number_integer()) {
          const int count = it.value().get<int>();
          if (count > 0) {
            items[it.key()] = count;
          }
        }
      }
    }
    if (channel.empty() || channel.find(':') != std::string::npos || items.empty() ||
        scheduled_at <= static_cast<int64_t>(std::time(nullptr))) {
      res.status = 400;
      res.set_content(R"({"error":"valid channel, items and future scheduled_at required"})",
                      "application/json; charset=utf-8");
      return;
    }
    const std::string id =
        mail::schedule_global(channel, update::get_huo_dong(), items, scheduled_at);
    if (id.empty()) {
      res.status = 503;
      res.set_content(R"({"error":"redis schedule write failed"})",
                      "application/json; charset=utf-8");
      return;
    }
    res.set_content(json{{"ok", true},
                         {"id", id},
                         {"channel", channel},
                         {"scheduled_at", scheduled_at},
                         {"scheduled_at_text", format_time(scheduled_at)}}
                        .dump(),
                    "application/json; charset=utf-8");
  });

  server.Delete("/admin/api/global-mail/schedules",
                [](const httplib::Request& req, httplib::Response& res) {
    if (!req.has_param("id") || !mail::cancel_global_schedule(req.get_param_value("id"))) {
      res.status = 404;
      res.set_content(R"({"error":"schedule not found"})", "application/json; charset=utf-8");
      return;
    }
    res.set_content(R"({"ok":true})", "application/json; charset=utf-8");
  });

  server.Post("/admin/api/save-key", [](const httplib::Request& req, httplib::Response& res) {
    json body;
    try {
      body = req.body.empty() ? json::object() : json::parse(req.body);
    } catch (...) {
      res.status = 400;
      res.set_content(R"({"error":"invalid json"})", "application/json; charset=utf-8");
      return;
    }
    const int save_index = body.value("save_index", 1);
    const std::string key = body.value("key", "");
    if (key.size() < 15) {
      res.status = 400;
      res.set_content(R"({"error":"key must be >= 15 chars"})", "application/json; charset=utf-8");
      return;
    }
    save_crypto::set_override_key(save_index, key);
    res.set_content(json{{"ok", true}, {"save_index", save_index}, {"key", key.substr(0, 16)}}.dump(),
                    "application/json; charset=utf-8");
  });

  server.Get("/admin/api/save-keys", [](const httplib::Request&, httplib::Response& res) {
    json out = json{{"items", json::array()}};
    for (int idx = 1; idx <= 63; ++idx) {
      if (auto key = save_crypto::get_override_key(idx)) {
        out["items"].push_back({{"save_index", idx}, {"key", *key}});
      }
    }
    res.set_content(out.dump(2), "application/json; charset=utf-8");
  });

  server.Get("/admin/api/push/online", [](const httplib::Request&, httplib::Response& res) {
    json items = json::array();
    for (const auto& client : push_hub::list_online()) {
      items.push_back({{"acc", client.acc},
                       {"peer", client.peer},
                       {"map", client.map},
                       {"nick", client.nick},
                       {"x", client.x},
                       {"y", client.y},
                       {"connected_at", client.connected_at},
                       {"connected_at_text", format_time(client.connected_at)},
                       {"last_active", client.last_active},
                       {"last_active_text", format_time(client.last_active)}});
    }
    res.set_content(json{{"count", items.size()}, {"items", items}}.dump(),
                    "application/json; charset=utf-8");
  });

  server.Post("/admin/api/push/send", [](const httplib::Request& req, httplib::Response& res) {
    json body;
    try {
      body = json::parse(req.body.empty() ? "{}" : req.body);
    } catch (...) {
      res.status = 400;
      res.set_content(R"({"error":"invalid json"})", "application/json; charset=utf-8");
      return;
    }
    const std::string acc = body.value("acc", "");
    const std::string msg = body.value("msg", "hello");
    if (acc.empty()) {
      res.status = 400;
      res.set_content(R"({"error":"acc required"})", "application/json; charset=utf-8");
      return;
    }
    std::string error;
    if (!push_hub::push_to_acc(acc, msg, &error)) {
      res.status = 404;
      res.set_content(json{{"ok", false}, {"error", error}}.dump(),
                      "application/json; charset=utf-8");
      return;
    }
    res.set_content(json{{"ok", true}, {"acc", acc}, {"msg", msg}}.dump(),
                    "application/json; charset=utf-8");
  });

  update::register_admin_routes(server, config);
}

}  // namespace jh::admin
