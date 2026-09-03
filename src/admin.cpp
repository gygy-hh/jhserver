#include "jh/admin.hpp"

#include "jh/auth.hpp"
#include "jh/config.hpp"

#include "jh/crypto.hpp"
#include "jh/mail.hpp"
#include "jh/save_crypto.hpp"
#include "jh/storage.hpp"
#include "jh/update.hpp"

#include <ctime>
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

}  // namespace

void register_routes(httplib::Server& server, const ServerConfig& config) {
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
                       {"created_at", acc.created_at},
                       {"created_at_text", format_time(acc.created_at)}});
    }
    res.set_content(json{{"items", items}}.dump(), "application/json; charset=utf-8");
  });

  server.Get("/admin/api/saves", [](const httplib::Request&, httplib::Response& res) {
    json items = json::array();
    for (const auto& save : storage::list_saves()) {
      items.push_back({{"acc", save.acc},
                       {"area", save.area},
                       {"size", save.size},
                       {"size_text", format_size(save.size)},
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
    constexpr size_t kMaxPreview = 8000;
    std::string preview = *save;
    const bool truncated = preview.size() > kMaxPreview;
    if (truncated) {
      preview.resize(kMaxPreview);
      preview += "\n\n... [truncated] ...";
    }
    json j{{"acc", acc}, {"area", area}, {"size", save->size()}, {"preview", preview}, {"truncated", truncated}};
    res.set_content(j.dump(), "application/json; charset=utf-8");
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

  server.Get("/admin/api/mails", [](const httplib::Request& req, httplib::Response& res) {
    if (!req.has_param("acc")) {
      res.status = 400;
      res.set_content(R"({"error":"acc required"})", "application/json; charset=utf-8");
      return;
    }
    const std::string acc = req.get_param_value("acc");
    int area = 1;
    if (req.has_param("area")) {
      area = std::stoi(req.get_param_value("area"));
    }
    json items = json::array();
    for (const auto& m : mail::list(acc, area)) {
      json props = json::object();
      for (const auto& [k, v] : m.items) {
        props[k] = v;
      }
      items.push_back({{"id", m.id},
                       {"desp", m.desp},
                       {"area", m.area},
                       {"items", props},
                       {"status", m.status},
                       {"push_version", m.push_version},
                       {"created_at", m.created_at},
                       {"created_at_text", format_time(m.created_at)}});
    }
    res.set_content(json{{"acc", acc}, {"area", area}, {"items", items}, {"pending", mail::pending_count(acc, area)}}.dump(2),
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
    const std::string acc = body.value("acc", "");
    if (acc.empty()) {
      res.status = 400;
      res.set_content(R"({"error":"acc required"})", "application/json; charset=utf-8");
      return;
    }
    const int area = body.value("area", 0);
    if (area <= 0) {
      res.status = 400;
      res.set_content(R"({"error":"area required"})", "application/json; charset=utf-8");
      return;
    }
    const std::string desp = body.value("desp", body.value("description", "系统邮件"));
    std::map<std::string, int> items;
    if (body.contains("items") && body["items"].is_object()) {
      for (auto it = body["items"].begin(); it != body["items"].end(); ++it) {
        items[it.key()] = it.value().get<int>();
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
    storage::ensure_account(acc);
    const std::string id = mail::send(acc, area, desp, items);
    res.set_content(json{{"ok", true}, {"id", id}, {"acc", acc}, {"area", area}}.dump(), "application/json; charset=utf-8");
  });

  server.Delete("/admin/api/mail", [](const httplib::Request& req, httplib::Response& res) {
    if (!req.has_param("acc") || !req.has_param("id")) {
      res.status = 400;
      res.set_content(R"({"error":"acc and id required"})", "application/json; charset=utf-8");
      return;
    }
    const std::string acc = req.get_param_value("acc");
    const std::string id = req.get_param_value("id");
    int area = 1;
    if (req.has_param("area")) {
      area = std::stoi(req.get_param_value("area"));
    }
    if (!mail::remove(acc, area, id)) {
      res.status = 404;
      res.set_content(R"({"error":"not found"})", "application/json; charset=utf-8");
      return;
    }
    res.set_content(R"({"ok":true})", "application/json; charset=utf-8");
  });

  server.Get("/admin/api/broadcasts", [](const httplib::Request&, httplib::Response& res) {
    json jobs = json::array();
    for (const auto& job : mail::list_broadcasts()) {
      json items = json::object();
      for (const auto& [prop_id, count] : job.items) {
        items[prop_id] = count;
      }
      jobs.push_back({
          {"id", job.id},
          {"desp", job.desp},
          {"items", items},
          {"scheduled_at", job.scheduled_at},
          {"scheduled_at_text", format_time(job.scheduled_at)},
          {"created_at", job.created_at},
          {"completed_at", job.completed_at},
          {"recipient_count", job.recipient_count},
          {"sent_count", job.sent_count},
          {"status", job.status},
      });
    }
    res.set_content(json{{"items", jobs}}.dump(), "application/json; charset=utf-8");
  });

  server.Post("/admin/api/broadcasts", [](const httplib::Request& req, httplib::Response& res) {
    json body;
    try {
      body = req.body.empty() ? json::object() : json::parse(req.body);
    } catch (...) {
      res.status = 400;
      res.set_content(R"({"error":"invalid json"})", "application/json; charset=utf-8");
      return;
    }
    std::map<std::string, int> items;
    if (body.contains("items") && body["items"].is_object()) {
      for (auto it = body["items"].begin(); it != body["items"].end(); ++it) {
        const int count = it.value().get<int>();
        if (count > 0) {
          items[it.key()] = count;
        }
      }
    }
    if (items.empty()) {
      res.status = 400;
      res.set_content(R"({"error":"items required"})", "application/json; charset=utf-8");
      return;
    }
    const int64_t scheduled_at = body.value("scheduled_at", static_cast<int64_t>(std::time(nullptr)));
    const std::string id = mail::create_broadcast(body.value("desp", "全服奖励"), items, scheduled_at);
    res.set_content(json{{"ok", true}, {"id", id}, {"scheduled_at", scheduled_at}}.dump(),
                    "application/json; charset=utf-8");
  });

  server.Delete("/admin/api/broadcasts", [](const httplib::Request& req, httplib::Response& res) {
    if (!req.has_param("id") || !mail::cancel_broadcast(req.get_param_value("id"))) {
      res.status = 404;
      res.set_content(R"({"error":"scheduled job not found"})", "application/json; charset=utf-8");
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

  update::register_admin_routes(server, config);
}

}  // namespace jh::admin
