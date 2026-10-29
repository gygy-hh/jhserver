#include "jh/lunjian.hpp"

#include "jh/config.hpp"
#include "jh/db.hpp"
#include "jh/lunjian_pool.hpp"
#include "jh/save_blob.hpp"
#include "jh/save_crypto.hpp"
#include "jh/storage.hpp"

#include <filesystem>

#include <algorithm>
#include <fstream>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace jh::lunjian {

namespace {

using json = nlohmann::json;

constexpr int kDefaultTimes = 25;
constexpr const char* kNpcNicks[] = {
    "琳琅斌", "蒺藜",       "云壤",   "明月共此时", "逍遥行", "穗穗",     "忘川",
    "天涯照月", "凤雨天蚀", "云烟飘渺", "十年不出武穆", "读博财富自由", "九刀下天山", "李清月",
    "萧暮雨", "忘绝情",   "慕容莹儿", "酒已烈",   "山水依在", "一日之寒",   "岁月落柏",
};
constexpr int kNpcCount = static_cast<int>(sizeof(kNpcNicks) / sizeof(kNpcNicks[0]));
constexpr int kMaxTeam = 5;
constexpr int64_t kFreshSec = 3LL * 24 * 60 * 60;
constexpr int kFightLevel = 90;
constexpr int kKungFuLevel = 9;
constexpr int kEquipSlots = 4;
// fight_data 生成规则变化时递增，旧版 NPC/真人会按新规则重建。
// v3：每人必须有合法武功，否则客户端 JhPerson::getHp 会空指针闪退。
// v4：武功/内功池内一人独占；NPC 阵容按互斥重roll。
constexpr int kFightDataVer = 4;

std::mutex g_mu;
Catalog g_catalog;
WeekPool g_pool;
// data/lunjian_pin.json {"week_id":N}：存在时池子和榜单固定在该期，不随每周一 04:00 自动换期。
int64_t g_pinned_week = 0;
bool g_ready = false;
std::string g_error = "论剑配置未加载";
std::unordered_map<std::string, std::string> g_ver;

struct Loadout {
  std::vector<int> chars;
  std::map<int, int> skill;
  std::map<int, int> nei;
  std::map<int, std::vector<int>> equip;
};

struct Fighter {
  bool found = false;
  std::string account;
  int area = 0;
  std::string nick;
  int head = 0;
  int fight_power = 0;
  int score = 0;
  int times_left = 0;
  int64_t times_day = 0;
  std::string fight_data;
  std::string loadout_json;
  bool is_npc = false;
  int64_t utime = 0;
};

std::string q(const std::string& text) { return "'" + db_escape(text) + "'"; }

std::string utf8_sql(const std::string& text) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string hex;
  hex.reserve(text.size() * 2);
  for (const unsigned char ch : text) {
    hex.push_back(kHex[ch >> 4]);
    hex.push_back(kHex[ch & 0x0F]);
  }
  return "CONVERT(UNHEX('" + hex + "') USING utf8mb4)";
}

int64_t now_unix() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

int64_t day_id(int64_t now) { return (now + 4 * 60 * 60) / 86400; }

std::string format_cst(int64_t unix_time) {
  const std::time_t shifted = static_cast<std::time_t>(unix_time + 8 * 60 * 60);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &shifted);
#else
  gmtime_r(&shifted, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
  return buf;
}

std::string clip_nick(std::string nick) {
  if (nick.size() > 60) {
    size_t cut = 60;
    while (cut > 0 && cut < nick.size() && (static_cast<unsigned char>(nick[cut]) & 0xC0) == 0x80) {
      --cut;
    }
    nick.resize(cut);
  }
  return nick.empty() ? "论剑客" : nick;
}

// 排行榜/匹配展示用：不影响 fight_data 阵容。名字按人机序号取，头像只在主角里抽。
uint32_t display_hash(const std::string& text) {
  uint32_t h = 2166136261u;
  for (const unsigned char ch : text) {
    h ^= ch;
    h *= 16777619u;
  }
  return h;
}

int npc_slot_of(const std::string& account) {
  const auto pos = account.rfind(':');
  if (pos == std::string::npos || pos + 1 >= account.size()) {
    return -1;
  }
  int index = 0;
  for (size_t i = pos + 1; i < account.size(); ++i) {
    const char ch = account[i];
    if (ch < '0' || ch > '9') {
      return -1;
    }
    index = index * 10 + (ch - '0');
    if (index >= 1000) {
      return -1;
    }
  }
  return index;
}

std::string ai_display_nick(const std::string& account) {
  int index = npc_slot_of(account);
  if (index < 0 || index >= kNpcCount) {
    index = static_cast<int>(display_hash(account) % static_cast<unsigned>(kNpcCount));
  }
  return kNpcNicks[index];
}

int ai_display_head(const std::string& account) {
  // 创角主角：男性 101-105，女性 121-125。同一人机每次看到的头像不变。
  static constexpr int kHeads[] = {101, 102, 103, 104, 105, 121, 122, 123, 124, 125};
  return kHeads[display_hash(account + "#head") % (sizeof(kHeads) / sizeof(kHeads[0]))];
}

std::string trim_copy(std::string text) {
  auto not_space = [](unsigned char ch) { return std::isspace(ch) == 0; };
  text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
  text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
  return text;
}

bool nick_is_placeholder(const std::string& nick, const std::string& account) {
  const std::string trimmed = trim_copy(nick);
  return trimmed.empty() || trimmed == account;
}

struct SaveIdentity {
  std::string nick;
  int head = 0;
};

std::unordered_map<std::string, SaveIdentity> g_save_identity;

int json_role_id(const json& doc, const char* key) {
  const auto it = doc.find(key);
  if (it == doc.end() || it->is_null()) {
    return 0;
  }
  if (it->is_number_integer()) {
    return it->get<int>();
  }
  if (it->is_number()) {
    return static_cast<int>(it->get<double>());
  }
  if (it->is_string()) {
    try {
      return std::stoi(it->get<std::string>());
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

void fill_identity_from_area(const std::string& account, int area, SaveIdentity& identity) {
  if (const auto meta = storage::get_save_meta(account, area)) {
    const std::string username = trim_copy(meta->username);
    if (identity.nick.empty() && !username.empty() && username != account) {
      identity.nick = username;
    }
  }
  if (!identity.nick.empty() && identity.head > 0) {
    return;
  }
  if (const auto blob = storage::load_cloud(account, area)) {
    if (const auto cipher = save_blob::get_segment(*blob, "dat.json");
        cipher && !cipher->empty() && *cipher != "null") {
      try {
        const json doc = json::parse(save_crypto::decrypt_dat_auto(*cipher, area, nullptr));
        if (identity.nick.empty() && doc.contains("mainNick") && doc["mainNick"].is_string()) {
          const std::string nick = trim_copy(doc["mainNick"].get<std::string>());
          if (!nick.empty() && nick != account) {
            identity.nick = nick;
          }
        }
        if (identity.head <= 0) {
          const int head = json_role_id(doc, "mainRole");
          if (head > 0) {
            identity.head = head;
          }
        }
      } catch (const std::exception& ex) {
        std::cerr << "[lunjian] save identity skipped acc=" << account << " area=" << area << " " << ex.what()
                  << std::endl;
      }
    }
  }
}

int head_from_fight_blob(const std::string& blob) {
  if (blob.empty()) {
    return 0;
  }
  const json fight = json::parse(blob, nullptr, false);
  if (!fight.is_object()) {
    return 0;
  }
  const auto& players = fight.contains("players") ? fight["players"] : json::array();
  if (!players.is_array() || players.empty() || !players[0].is_object()) {
    return 0;
  }
  return players[0].value("head", 0);
}

int head_from_loadout_blob(const std::string& text) {
  if (text.empty()) {
    return 0;
  }
  const json body = json::parse(text, nullptr, false);
  if (!body.is_object() || !body.contains("chars") || !body["chars"].is_array() || body["chars"].empty()) {
    return 0;
  }
  const auto& first = body["chars"][0];
  if (first.is_number_integer()) {
    return first.get<int>();
  }
  if (first.is_number()) {
    return static_cast<int>(first.get<double>());
  }
  return 0;
}

SaveIdentity identity_from_save(const std::string& account, int area) {
  const std::string key = account + "#" + std::to_string(area);
  if (const auto it = g_save_identity.find(key); it != g_save_identity.end()) {
    return it->second;
  }
  SaveIdentity identity;
  fill_identity_from_area(account, area, identity);
  // 当前存档被删/未上传时，用同账号其它存档补昵称和头像。
  if (identity.nick.empty() || identity.head <= 0) {
    int64_t best_ts = -1;
    SaveIdentity sibling;
    for (int other = 1; other <= 66; ++other) {
      if (other == area) {
        continue;
      }
      const auto meta = storage::get_save_meta(account, other);
      if (!meta) {
        continue;
      }
      const int64_t ts = std::max(meta->manual_upload_at, meta->save_time);
      if (ts < best_ts) {
        continue;
      }
      SaveIdentity candidate;
      fill_identity_from_area(account, other, candidate);
      if (candidate.nick.empty() && candidate.head <= 0) {
        continue;
      }
      best_ts = ts;
      sibling = std::move(candidate);
    }
    if (identity.nick.empty() && !sibling.nick.empty()) {
      identity.nick = sibling.nick;
    }
    if (identity.head <= 0 && sibling.head > 0) {
      identity.head = sibling.head;
    }
  }
  if (!identity.nick.empty() || identity.head > 0) {
    g_save_identity[key] = identity;
  }
  return identity;
}

void apply_display_identity(Fighter& fighter) {
  if (fighter.is_npc) {
    fighter.nick = ai_display_nick(fighter.account);
    fighter.head = ai_display_head(fighter.account);
    return;
  }
  const bool nick_bad = nick_is_placeholder(fighter.nick, fighter.account);
  const bool head_bad = fighter.head <= 0;
  if (!nick_bad && !head_bad) {
    return;
  }
  if (fighter.account.empty() || fighter.area <= 0) {
    return;
  }
  const SaveIdentity identity = identity_from_save(fighter.account, fighter.area);
  if (nick_bad && !identity.nick.empty()) {
    fighter.nick = identity.nick;
  }
  if (head_bad) {
    if (identity.head > 0) {
      fighter.head = identity.head;
    } else if (const int from_fight = head_from_fight_blob(fighter.fight_data); from_fight > 0) {
      fighter.head = from_fight;
    } else if (const int from_loadout = head_from_loadout_blob(fighter.loadout_json); from_loadout > 0) {
      fighter.head = from_loadout;
    }
  }
}

void persist_identity(const std::string& channel, const Fighter& fighter, const std::string& old_nick, int old_head) {
  if (fighter.is_npc || fighter.account.empty() || fighter.area <= 0) {
    return;
  }
  const bool nick_fixed =
      nick_is_placeholder(old_nick, fighter.account) && !nick_is_placeholder(fighter.nick, fighter.account);
  const bool head_fixed = old_head <= 0 && fighter.head > 0;
  if (!nick_fixed && !head_fixed) {
    return;
  }
  db_exec("UPDATE lunjian_fighter SET nick=" + utf8_sql(clip_nick(fighter.nick)) +
          ", head=" + std::to_string(fighter.head) + " WHERE channel=" + q(channel) + " AND account=" +
          q(fighter.account) + " AND area=" + std::to_string(fighter.area) +
          " AND week_id=" + std::to_string(g_pool.week_id));
  std::cerr << "[lunjian] identity acc=" << fighter.account << " area=" << fighter.area << " nick=" << fighter.nick
            << " head=" << fighter.head << std::endl;
}

std::string new_ver() {
  static const char* kHex = "0123456789abcdef";
  std::random_device rd;
  std::string out(16, '0');
  for (char& ch : out) {
    ch = kHex[rd() % 16];
  }
  return out;
}

int json_int(const json& body, const char* key, int fallback = 0) {
  const auto it = body.find(key);
  if (it == body.end() || it->is_null()) {
    return fallback;
  }
  if (it->is_number_integer()) {
    return it->get<int>();
  }
  if (it->is_number()) {
    return static_cast<int>(it->get<double>());
  }
  if (it->is_string()) {
    try {
      return std::stoi(it->get<std::string>());
    } catch (...) {
      return fallback;
    }
  }
  return fallback;
}

std::string json_str(const json& body, const char* key) {
  const auto it = body.find(key);
  if (it == body.end() || it->is_null()) {
    return {};
  }
  if (it->is_string()) {
    return it->get<std::string>();
  }
  if (it->is_number_integer()) {
    return std::to_string(it->get<long long>());
  }
  return {};
}

json fail(const std::string& msg, int code = 99) {
  return json{{"code", code}, {"msg", msg}, {"result", msg}};
}

json item_json(const PoolItem& item) {
  return json{{"id", item.id},     {"nick", item.nick}, {"star", item.star}, {"graded", item.graded},
              {"head", item.head}, {"type", item.type}, {"sex", item.sex},   {"icon", item.icon},
              {"quality", item.quality}};
}

json list_json(const std::vector<PoolItem>& items) {
  json out = json::array();
  for (const auto& item : items) {
    out.push_back(item_json(item));
  }
  return out;
}

json id_list(const std::vector<PoolItem>& items) {
  json out = json::array();
  for (const auto& item : items) {
    out.push_back(item.id);
  }
  return out;
}

json pool_json(const WeekPool& pool) {
  return json{{"week", pool.week_id},
              {"beginAt", format_cst(pool.begin_unix)},
              {"endAt", format_cst(pool.end_unix)},
              {"equipGrade", pool.equip_grade},
              {"skillGrade", pool.skill_grade},
              {"neiGrade", pool.nei_grade},
              {"charIds", id_list(pool.chars)},
              {"equipIds", id_list(pool.equips)},
              {"skillIds", id_list(pool.skills)},
              {"neiIds", id_list(pool.neis)},
              {"chars", list_json(pool.chars)},
              {"equips", list_json(pool.equips)},
              {"skills", list_json(pool.skills)},
              {"neiGongs", list_json(pool.neis)},
              {"open",
               json{{"btn_char", json{{"screen", "char"}, {"ui", "ui_char_change.csb"}}},
                    {"btn_skill_0", json{{"screen", "skill"}, {"ui", "ui_skill2.csb"}}},
                    {"btn_skill", json{{"screen", "equip"}, {"ui", "ui_char_equipchange.csb"}}}}}};
}

int64_t load_pinned_week(const std::string& data_dir) {
  std::ifstream in(data_dir + "/lunjian_pin.json");
  if (!in) {
    return 0;
  }
  const json pin = json::parse(in, nullptr, false);
  if (!pin.is_object()) {
    throw std::runtime_error("lunjian_pin.json invalid");
  }
  const int64_t week = pin.value("week_id", static_cast<int64_t>(0));
  if (week <= 0) {
    throw std::runtime_error("lunjian_pin.json week_id invalid");
  }
  return week;
}

void refresh_pool() {
  const int64_t natural = week_id_at(now_unix());
  const int64_t week = g_pinned_week > 0 ? g_pinned_week : natural;
  if (g_pool.week_id == week && !g_pool.chars.empty()) {
    return;
  }
  g_pool = build_week_pool(g_catalog, week);
  if (g_pinned_week > 0) {
    // 固定期的结束时间顺延到自然周末尾，避免客户端倒计时显示已结束。
    int64_t begin = 0;
    int64_t end = 0;
    week_bounds(natural, begin, end);
    g_pool.end_unix = std::max(g_pool.end_unix, end);
  }
  auto graded = [](const std::vector<PoolItem>& items) {
    int count = 0;
    int star = 0;
    for (const auto& item : items) {
      if (!item.graded) {
        continue;
      }
      ++count;
      if (star == 0) {
        star = item.star;
      } else if (item.star != star) {
        throw std::runtime_error("lunjian graded stars diverged");
      }
    }
    if (count > kPoolGradedMax) {
      throw std::runtime_error("lunjian graded count exceeded");
    }
  };
  graded(g_pool.equips);
  graded(g_pool.skills);
  graded(g_pool.neis);
}

std::string ver_key(const std::string& channel) {
  return channel + ":" + std::to_string(g_pool.week_id);
}

std::string ver_of(const std::string& channel) {
  const std::string key = ver_key(channel);
  auto it = g_ver.find(key);
  if (it == g_ver.end() || it->second.empty()) {
    it = g_ver.insert_or_assign(key, new_ver()).first;
  }
  return it->second;
}

void bump_ver(const std::string& channel) { g_ver[ver_key(channel)] = new_ver(); }

void ensure_schema() {
  db_exec("SET NAMES utf8mb4");
  db_exec(R"(
    CREATE TABLE IF NOT EXISTS lunjian_fighter (
      id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
      channel VARCHAR(64) NOT NULL,
      account VARCHAR(128) NOT NULL,
      area INT NOT NULL,
      week_id BIGINT NOT NULL,
      nick VARCHAR(64) NOT NULL DEFAULT '',
      head INT NOT NULL DEFAULT 0,
      fight_power INT NOT NULL DEFAULT 0,
      score INT NOT NULL DEFAULT 0,
      times_left INT NOT NULL DEFAULT 25,
      times_day BIGINT NOT NULL DEFAULT 0,
      fight_data MEDIUMTEXT,
      loadout_json TEXT,
      is_npc TINYINT NOT NULL DEFAULT 0,
      utime BIGINT NOT NULL DEFAULT 0,
      PRIMARY KEY (id),
      UNIQUE KEY uk_lj (channel, account, area, week_id),
      KEY idx_lj_match (channel, week_id, score)
    ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
  )");
  // 旧服上该表可能由早期默认字符集创建。CREATE IF NOT EXISTS 不会修正已有列，
  // 中文 NPC/玩家昵称会让整个池子请求在写入阵容时失败。
  db_exec(
      "ALTER TABLE lunjian_fighter CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci");
}

int cell_int(const std::vector<std::string>& row, size_t index) {
  if (index >= row.size() || row[index].empty()) {
    return 0;
  }
  try {
    return std::stoi(row[index]);
  } catch (...) {
    return 0;
  }
}

int64_t cell_i64(const std::vector<std::string>& row, size_t index) {
  if (index >= row.size() || row[index].empty()) {
    return 0;
  }
  try {
    return std::stoll(row[index]);
  } catch (...) {
    return 0;
  }
}

Fighter read_fighter(const std::vector<std::string>& row) {
  Fighter fighter;
  fighter.found = true;
  fighter.account = row.size() > 0 ? row[0] : "";
  fighter.area = cell_int(row, 1);
  fighter.nick = row.size() > 2 ? row[2] : "";
  fighter.head = cell_int(row, 3);
  fighter.fight_power = cell_int(row, 4);
  fighter.score = cell_int(row, 5);
  fighter.times_left = cell_int(row, 6);
  fighter.times_day = cell_i64(row, 7);
  fighter.fight_data = row.size() > 8 ? row[8] : "";
  fighter.loadout_json = row.size() > 9 ? row[9] : "";
  fighter.is_npc = cell_int(row, 10) != 0;
  fighter.utime = cell_i64(row, 11);
  return fighter;
}

const char* kFighterCols =
    "account, area, nick, head, fight_power, score, times_left, times_day, fight_data, loadout_json, is_npc, utime";

Fighter find_fighter(const std::string& channel, const std::string& account, int area, int64_t week) {
  const auto rows = db_query("SELECT " + std::string(kFighterCols) +
                             " FROM lunjian_fighter WHERE channel=" + q(channel) + " AND account=" + q(account) +
                             " AND area=" + std::to_string(area) + " AND week_id=" + std::to_string(week) + " LIMIT 1");
  if (rows.empty()) {
    return {};
  }
  return read_fighter(rows[0]);
}

void reset_times_if_needed(const std::string& channel, Fighter& fighter, int64_t week, int64_t today) {
  if (!fighter.found || fighter.is_npc || fighter.times_day == today) {
    return;
  }
  fighter.times_left = kDefaultTimes;
  fighter.times_day = today;
  db_exec("UPDATE lunjian_fighter SET times_left=" + std::to_string(kDefaultTimes) +
          ", times_day=" + std::to_string(today) + " WHERE channel=" + q(channel) +
          " AND account=" + q(fighter.account) + " AND area=" + std::to_string(fighter.area) +
          " AND week_id=" + std::to_string(week));
}

Loadout parse_loadout(const std::string& text) {
  Loadout loadout;
  if (text.empty()) {
    return loadout;
  }
  const json body = json::parse(text, nullptr, false);
  if (!body.is_object()) {
    return loadout;
  }
  if (body.contains("chars") && body["chars"].is_array()) {
    for (const auto& id : body["chars"]) {
      if (id.is_number_integer()) {
        loadout.chars.push_back(id.get<int>());
      }
    }
  }
  auto read_map = [](const json& body, const char* key, std::map<int, int>& out) {
    if (!body.contains(key) || !body[key].is_object()) {
      return;
    }
    for (auto it = body[key].begin(); it != body[key].end(); ++it) {
      try {
        const int char_id = std::stoi(it.key());
        const int value = it.value().is_number_integer() ? it.value().get<int>() : 0;
        if (char_id > 0 && value > 0) {
          out[char_id] = value;
        }
      } catch (...) {
      }
    }
  };
  read_map(body, "skill", loadout.skill);
  read_map(body, "nei", loadout.nei);
  if (body.contains("equip") && body["equip"].is_object()) {
    for (auto it = body["equip"].begin(); it != body["equip"].end(); ++it) {
      try {
        const int char_id = std::stoi(it.key());
        if (char_id <= 0 || !it.value().is_array()) {
          continue;
        }
        std::vector<int> equips;
        for (const auto& eid : it.value()) {
          if (eid.is_number_integer() && eid.get<int>() > 0) {
            equips.push_back(eid.get<int>());
          }
        }
        if (!equips.empty()) {
          loadout.equip[char_id] = std::move(equips);
        }
      } catch (...) {
      }
    }
  }
  return loadout;
}

json loadout_json(const Loadout& loadout) {
  json skill = json::object();
  json nei = json::object();
  json equip = json::object();
  for (const auto& [id, value] : loadout.skill) {
    skill[std::to_string(id)] = value;
  }
  for (const auto& [id, value] : loadout.nei) {
    nei[std::to_string(id)] = value;
  }
  for (const auto& [id, values] : loadout.equip) {
    equip[std::to_string(id)] = values;
  }
  return json{{"chars", loadout.chars}, {"skill", skill}, {"nei", nei}, {"equip", equip}};
}

// first.*=true 表示该栏仍未配齐。必须每人一份，不能“有一个人配了就算完成”。
json first_flags(const Loadout& loadout) {
  if (loadout.chars.empty()) {
    return json{{"char", true}, {"skill", true}, {"equip", true}, {"nei", true}};
  }
  bool need_skill = false;
  bool need_nei = false;
  bool need_equip = false;
  for (int id : loadout.chars) {
    const auto sk = loadout.skill.find(id);
    if (sk == loadout.skill.end() || find_item(g_pool.skills, sk->second) == nullptr) {
      need_skill = true;
    }
    const auto ng = loadout.nei.find(id);
    if (ng == loadout.nei.end() || find_item(g_pool.neis, ng->second) == nullptr) {
      need_nei = true;
    }
    if (!loadout.equip.count(id) || loadout.equip.at(id).empty()) {
      need_equip = true;
    }
  }
  return json{{"char", false}, {"skill", need_skill}, {"equip", need_equip}, {"nei", need_nei}};
}

int star_of(const std::vector<PoolItem>& items, int id) {
  const PoolItem* item = find_item(items, id);
  return item ? std::max(item->star, 1) : 1;
}

int fight_power_of(const Loadout& loadout) {
  int power = 5000;
  for (int id : loadout.chars) {
    power += 2000;
    if (const auto it = loadout.skill.find(id); it != loadout.skill.end()) {
      power += 600 * star_of(g_pool.skills, it->second);
    }
    if (const auto it = loadout.nei.find(id); it != loadout.nei.end()) {
      power += 600 * star_of(g_pool.neis, it->second);
    }
    if (const auto it = loadout.equip.find(id); it != loadout.equip.end()) {
      for (int eid : it->second) {
        power += 200 * star_of(g_pool.equips, eid);
      }
    }
  }
  return power;
}

// 与客户端 JhInfo::getEquipPosFromType 一致：拳剑刀棍→武器，5 衣服，6 戒指，7 护腕。
int equip_slot(int eid) {
  const PoolItem* item = find_item(g_pool.equips, eid);
  if (!item) {
    return -1;
  }
  if (item->type >= 1 && item->type <= 4) {
    return 0;
  }
  if (item->type >= 5 && item->type <= 7) {
    return item->type - 4;
  }
  return -1;
}

// 客户端按 equip[槽位] 读取，必须按槽位落位，同槽位只保留第一件。
std::array<int, kEquipSlots> slotted_equips(const std::vector<int>& ids) {
  std::array<int, kEquipSlots> slots{};
  for (int eid : ids) {
    const int slot = equip_slot(eid);
    if (slot >= 0 && slots[static_cast<size_t>(slot)] == 0) {
      slots[static_cast<size_t>(slot)] = eid;
    }
  }
  return slots;
}

int default_pool_id(const std::vector<PoolItem>& items) {
  return items.empty() ? 0 : items.front().id;
}

bool kungfu_used_by_other(const std::map<int, int>& owned, int char_id, int item_id) {
  if (item_id <= 0) {
    return false;
  }
  for (const auto& [owner, value] : owned) {
    if (owner != char_id && value == item_id) {
      return true;
    }
  }
  return false;
}

// 同一武功/内功只能给阵容里一个人；已被他人占用时禁止再选（不踢人）。
bool try_assign_kungfu(std::map<int, int>& owned, int char_id, int item_id) {
  if (item_id <= 0) {
    owned.erase(char_id);
    return true;
  }
  if (kungfu_used_by_other(owned, char_id, item_id)) {
    return false;
  }
  owned[char_id] = item_id;
  return true;
}

int take_unused_kungfu(const std::vector<PoolItem>& pool, const std::map<int, int>& owned) {
  std::unordered_map<int, bool> taken;
  for (const auto& [_, id] : owned) {
    if (id > 0) {
      taken[id] = true;
    }
  }
  for (const auto& item : pool) {
    if (item.id > 0 && !taken[item.id]) {
      return item.id;
    }
  }
  return 0;
}

// 选人后补齐缺项；池内武功/内功一人独占，避免多人共用同一 id。
bool ensure_loadout_kungfu(Loadout& loadout) {
  bool changed = false;
  std::map<int, int> skills;
  std::map<int, int> neis;
  for (int id : loadout.chars) {
    if (const auto sk = loadout.skill.find(id);
        sk != loadout.skill.end() && find_item(g_pool.skills, sk->second) != nullptr &&
        !kungfu_used_by_other(skills, id, sk->second)) {
      skills[id] = sk->second;
    }
    if (const auto ng = loadout.nei.find(id);
        ng != loadout.nei.end() && find_item(g_pool.neis, ng->second) != nullptr &&
        !kungfu_used_by_other(neis, id, ng->second)) {
      neis[id] = ng->second;
    }
  }
  for (int id : loadout.chars) {
    if (!skills.count(id)) {
      if (const int skill = take_unused_kungfu(g_pool.skills, skills)) {
        skills[id] = skill;
        changed = true;
      }
    }
    if (!neis.count(id)) {
      if (const int nei = take_unused_kungfu(g_pool.neis, neis)) {
        neis[id] = nei;
        changed = true;
      }
    }
  }
  if (skills != loadout.skill || neis != loadout.nei) {
    loadout.skill = std::move(skills);
    loadout.nei = std::move(neis);
    changed = true;
  }
  return changed;
}

// 战斗侧 JhPerson::getHp 会无条件读武功模板；skill==0 或非法 id 会闪退。
int resolve_fight_skill(int char_id, const Loadout& loadout) {
  if (const auto it = loadout.skill.find(char_id); it != loadout.skill.end()) {
    if (find_item(g_pool.skills, it->second) != nullptr) {
      return it->second;
    }
  }
  return take_unused_kungfu(g_pool.skills, loadout.skill);
}

int resolve_fight_nei(int char_id, const Loadout& loadout) {
  if (const auto it = loadout.nei.find(char_id); it != loadout.nei.end()) {
    if (find_item(g_pool.neis, it->second) != nullptr) {
      return it->second;
    }
  }
  return take_unused_kungfu(g_pool.neis, loadout.nei);
}

json player_json(int char_id, const Loadout& loadout) {
  const PoolItem* person = find_item(g_pool.chars, char_id);
  const int skill = resolve_fight_skill(char_id, loadout);
  const int nei = resolve_fight_nei(char_id, loadout);
  json equips = json::array();
  const auto slots =
      slotted_equips(loadout.equip.count(char_id) ? loadout.equip.at(char_id) : std::vector<int>{});
  for (int eid : slots) {
    equips.push_back(json{{"eid", eid}, {"lev", eid > 0 ? 1 : 0}, {"add", 0}, {"tx", 0}, {"txType", 0}});
  }
  return json{{"lev", kFightLevel},
              {"exp", 0},
              {"skill", skill},
              {"skillLevel", skill > 0 ? kKungFuLevel : 1},
              {"skill2", json::object()},
              {"neiGong", nei},
              {"neiGong2", json::object()},
              {"neiGongLevel", nei > 0 ? kKungFuLevel : 1},
              {"equip", equips},
              {"head", char_id},
              {"drug", json::object()},
              {"drugTime", json::object()},
              {"sex", person ? person->sex : 1}};
}

std::string fight_data_of(const Loadout& loadout) {
  json players = json::array();
  json tibus = json::array();
  for (size_t i = 0; i < loadout.chars.size(); ++i) {
    json player = player_json(loadout.chars[i], loadout);
    if (i < static_cast<size_t>(kMaxTeam)) {
      players.push_back(std::move(player));
    } else {
      tibus.push_back(std::move(player));
    }
  }
  return json{{"players", players}, {"tibus", tibus}, {"fParams", json::object()}, {"ljVer", kFightDataVer}}
      .dump();
}

void save_fighter(const std::string& channel, const Fighter& fighter, int64_t week) {
  const std::string sql = "INSERT INTO lunjian_fighter (channel, account, area, week_id, nick, head, fight_power, "
                          "score, times_left, times_day, fight_data, loadout_json, is_npc, utime) VALUES (" +
                          q(channel) + "," + q(fighter.account) + "," + std::to_string(fighter.area) + "," +
                          std::to_string(week) + "," + utf8_sql(clip_nick(fighter.nick)) + "," +
                          std::to_string(fighter.head) + "," + std::to_string(fighter.fight_power) + "," +
                          std::to_string(fighter.score) + "," +
                          std::to_string(fighter.times_left) + "," + std::to_string(fighter.times_day) + "," +
                          q(fighter.fight_data) + "," + q(fighter.loadout_json) + "," +
                          std::to_string(fighter.is_npc ? 1 : 0) + "," + std::to_string(fighter.utime) +
                          ") ON DUPLICATE KEY UPDATE nick=VALUES(nick), head=VALUES(head), "
                          "fight_power=VALUES(fight_power), score=VALUES(score), times_left=VALUES(times_left), "
                          "times_day=VALUES(times_day), fight_data=VALUES(fight_data), loadout_json=VALUES(loadout_json), "
                          "utime=VALUES(utime)";
  db_exec(sql);
}

int pick_id(const std::vector<PoolItem>& items, std::mt19937& rng) {
  if (items.empty()) {
    return 0;
  }
  return items[rng() % items.size()].id;
}

bool fight_needs_rebuild(const json& fight);

bool npc_uses_current_pool(const std::string& blob) {
  const json fight = json::parse(blob, nullptr, false);
  if (fight_needs_rebuild(fight)) {
    return false;
  }
  auto equipped = [](const json& list) {
    if (!list.is_array() || list.empty()) {
      return false;
    }
    for (const auto& player : list) {
      if (!player.is_object() || !player.contains("equip") || !player["equip"].is_array() || player["equip"].empty()) {
        return false;
      }
      for (const auto& equip : player["equip"]) {
        const int eid = equip.is_object() ? equip.value("eid", 0) : 0;
        if (eid <= 0 || find_item(g_pool.equips, eid) == nullptr) {
          return false;
        }
      }
    }
    return true;
  };
  return equipped(fight.value("players", json::array()));
}

void ensure_npcs(const std::string& channel) {
  const auto rows = db_query("SELECT account, fight_data FROM lunjian_fighter WHERE channel=" + q(channel) +
                             " AND week_id=" + std::to_string(g_pool.week_id) + " AND is_npc=1");
  bool pool_ok = !rows.empty();
  std::unordered_set<int> have;
  for (const auto& row : rows) {
    const int index = npc_slot_of(row.empty() ? "" : row[0]);
    if (index >= 0 && index < kNpcCount) {
      have.insert(index);
    }
    const std::string blob = row.size() > 1 ? row[1] : "";
    if (blob.empty() || !npc_uses_current_pool(blob)) {
      pool_ok = false;
    }
  }
  bool complete = pool_ok;
  if (complete) {
    for (int i = 0; i < kNpcCount; ++i) {
      if (!have.count(i)) {
        complete = false;
        break;
      }
    }
  }
  if (complete) {
    return;
  }
  std::array<std::vector<int>, kEquipSlots> slot_pool;
  for (const auto& item : g_pool.equips) {
    if (const int slot = equip_slot(item.id); slot >= 0) {
      slot_pool[static_cast<size_t>(slot)].push_back(item.id);
    }
  }
  std::mt19937 rng(static_cast<std::mt19937::result_type>(
      static_cast<uint64_t>(g_pool.week_id) ^ (0x4E5Cu * static_cast<uint64_t>(kFightDataVer))));
  auto build_one = [&](int i) {
    if (pool_ok) {
      rng.seed(static_cast<std::mt19937::result_type>(
          static_cast<uint64_t>(g_pool.week_id) ^ (0xA11u * static_cast<uint64_t>(i + 1))));
    }
    std::vector<int> chars;
    for (const auto& item : g_pool.chars) {
      chars.push_back(item.id);
    }
    std::shuffle(chars.begin(), chars.end(), rng);
    const int team = std::min(kMaxTeam, static_cast<int>(chars.size()));
    std::vector<int> skill_bag;
    std::vector<int> nei_bag;
    for (const auto& item : g_pool.skills) {
      skill_bag.push_back(item.id);
    }
    for (const auto& item : g_pool.neis) {
      nei_bag.push_back(item.id);
    }
    std::shuffle(skill_bag.begin(), skill_bag.end(), rng);
    std::shuffle(nei_bag.begin(), nei_bag.end(), rng);
    Loadout loadout;
    for (int n = 0; n < team; ++n) {
      const int char_id = chars[static_cast<size_t>(n)];
      loadout.chars.push_back(char_id);
      if (!skill_bag.empty()) {
        loadout.skill[char_id] = skill_bag.back();
        skill_bag.pop_back();
      }
      if (!nei_bag.empty()) {
        loadout.nei[char_id] = nei_bag.back();
        nei_bag.pop_back();
      }
      std::vector<int> equips;
      for (const auto& candidates : slot_pool) {
        if (!candidates.empty()) {
          equips.push_back(candidates[rng() % candidates.size()]);
        }
      }
      if (!equips.empty()) {
        loadout.equip[char_id] = equips;
      }
    }
    ensure_loadout_kungfu(loadout);
    Fighter fighter;
    fighter.found = true;
    fighter.account = "npc:" + std::to_string(g_pool.week_id) + ":" + std::to_string(i);
    fighter.area = 1;
    fighter.is_npc = true;
    fighter.fight_power = fight_power_of(loadout);
    fighter.fight_data = fight_data_of(loadout);
    fighter.loadout_json = loadout_json(loadout).dump();
    fighter.utime = now_unix();
    fighter.times_left = 0;
    fighter.times_day = day_id(fighter.utime);
    apply_display_identity(fighter);
    return fighter;
  };
  if (pool_ok) {
    bool added = false;
    for (int i = 0; i < kNpcCount; ++i) {
      if (have.count(i)) {
        continue;
      }
      Fighter fighter = build_one(i);
      fighter.score = 6 + i * 3;
      save_fighter(channel, fighter, g_pool.week_id);
      added = true;
    }
    if (added) {
      bump_ver(channel);
    }
    return;
  }
  // 阵容不在本期池里时才整批重建。真人积分和阵容留在本期。
  db_exec("DELETE FROM lunjian_fighter WHERE channel=" + q(channel) +
          " AND week_id=" + std::to_string(g_pool.week_id) + " AND is_npc=1");
  struct Built {
    Fighter fighter;
  };
  std::vector<Built> built;
  built.reserve(kNpcCount);
  for (int i = 0; i < kNpcCount; ++i) {
    built.push_back(Built{build_one(i)});
  }
  std::sort(built.begin(), built.end(),
            [](const Built& a, const Built& b) { return a.fighter.fight_power < b.fighter.fight_power; });
  for (size_t i = 0; i < built.size(); ++i) {
    built[i].fighter.score = 6 + static_cast<int>(i) * 3;
    save_fighter(channel, built[i].fighter, g_pool.week_id);
  }
  bump_ver(channel);
}

Fighter ensure_player(const std::string& channel, const std::string& account, int area) {
  const int64_t today = day_id(now_unix());
  Fighter fighter = find_fighter(channel, account, area, g_pool.week_id);
  const bool created = !fighter.found;
  if (created) {
    fighter.found = true;
    fighter.account = account;
    fighter.area = area;
    fighter.nick = account;
    fighter.times_left = kDefaultTimes;
    fighter.times_day = today;
    fighter.utime = 0;
  }
  const std::string old_nick = fighter.nick;
  const int old_head = fighter.head;
  apply_display_identity(fighter);
  if (created) {
    save_fighter(channel, fighter, g_pool.week_id);
  } else {
    persist_identity(channel, fighter, old_nick, old_head);
    reset_times_if_needed(channel, fighter, g_pool.week_id, today);
  }
  return fighter;
}

bool in_pool(const std::vector<PoolItem>& items, int id) { return id > 0 && find_item(items, id) != nullptr; }

bool fight_blob_in_pool(const json& fight, Loadout& loadout) {
  loadout = {};
  auto take_players = [&](const json& list) {
    if (!list.is_array()) {
      return true;
    }
    for (const auto& player : list) {
      if (!player.is_object()) {
        return false;
      }
      const int head = player.value("head", 0);
      if (!in_pool(g_pool.chars, head)) {
        return false;
      }
      loadout.chars.push_back(head);
      const int skill = player.value("skill", 0);
      const int nei = player.value("neiGong", 0);
      if (skill && !in_pool(g_pool.skills, skill)) {
        return false;
      }
      if (nei && !in_pool(g_pool.neis, nei)) {
        return false;
      }
      if (skill) {
        loadout.skill[head] = skill;
      }
      if (nei) {
        loadout.nei[head] = nei;
      }
      std::vector<int> equips;
      if (player.contains("equip") && player["equip"].is_array()) {
        for (const auto& equip : player["equip"]) {
          const int eid = equip.is_object() ? equip.value("eid", 0) : 0;
          if (eid && !in_pool(g_pool.equips, eid)) {
            return false;
          }
          if (eid) {
            equips.push_back(eid);
          }
        }
      }
      if (!equips.empty()) {
        loadout.equip[head] = equips;
      }
    }
    return true;
  };
  if (!take_players(fight.value("players", json::array())) || !take_players(fight.value("tibus", json::array()))) {
    return false;
  }
  std::vector<int> unique_chars;
  for (int id : loadout.chars) {
    if (std::find(unique_chars.begin(), unique_chars.end(), id) == unique_chars.end()) {
      unique_chars.push_back(id);
    }
  }
  loadout.chars = std::move(unique_chars);
  return !loadout.chars.empty();
}

bool fight_needs_rebuild(const json& fight) {
  if (!fight.is_object() || fight.value("ljVer", 0) != kFightDataVer) {
    return true;
  }
  auto bad_players = [](const json& list) {
    if (!list.is_array()) {
      return false;
    }
    for (const auto& player : list) {
      if (!player.is_object()) {
        return true;
      }
      const int skill = player.value("skill", 0);
      if (skill <= 0 || find_item(g_pool.skills, skill) == nullptr) {
        return true;
      }
      const int nei = player.value("neiGong", 0);
      if (nei > 0 && find_item(g_pool.neis, nei) == nullptr) {
        return true;
      }
    }
    return false;
  };
  return bad_players(fight.value("players", json::array())) ||
         bad_players(fight.value("tibus", json::array()));
}

// 重建用：人物必须在池内；武功/内功/装备不在池内则丢弃，后续由 resolve_fight_* 填默认。
bool fight_blob_for_rebuild(const json& fight, Loadout& loadout) {
  loadout = {};
  auto take_players = [&](const json& list) {
    if (!list.is_array()) {
      return true;
    }
    for (const auto& player : list) {
      if (!player.is_object()) {
        continue;
      }
      const int head = player.value("head", 0);
      if (!in_pool(g_pool.chars, head)) {
        continue;
      }
      if (std::find(loadout.chars.begin(), loadout.chars.end(), head) != loadout.chars.end()) {
        continue;
      }
      loadout.chars.push_back(head);
      const int skill = player.value("skill", 0);
      const int nei = player.value("neiGong", 0);
      if (in_pool(g_pool.skills, skill)) {
        loadout.skill[head] = skill;
      }
      if (in_pool(g_pool.neis, nei)) {
        loadout.nei[head] = nei;
      }
      std::vector<int> equips;
      if (player.contains("equip") && player["equip"].is_array()) {
        for (const auto& equip : player["equip"]) {
          const int eid = equip.is_object() ? equip.value("eid", 0) : 0;
          if (in_pool(g_pool.equips, eid)) {
            equips.push_back(eid);
          }
        }
      }
      if (!equips.empty()) {
        loadout.equip[head] = equips;
      }
    }
    return true;
  };
  take_players(fight.value("players", json::array()));
  take_players(fight.value("tibus", json::array()));
  return !loadout.chars.empty();
}

// 旧数据可能是 60 级、装备未按槽位、或缺武功；下发前统一按当前规则重建。
std::string current_fight_data(const std::string& blob) {
  if (blob.empty()) {
    return blob;
  }
  const json fight = json::parse(blob, nullptr, false);
  if (!fight.is_object() || !fight_needs_rebuild(fight)) {
    return blob;
  }
  Loadout loadout;
  if (!fight_blob_for_rebuild(fight, loadout) && !fight_blob_in_pool(fight, loadout)) {
    return blob;
  }
  return fight_data_of(loadout);
}

json base_board(const Fighter& me, const std::string& channel) {
  const Loadout loadout = parse_loadout(me.loadout_json);
  return json{{"code", 0},
              {"msg", "华山论剑每周一凌晨4点轮换人物、装备、内功和武功"},
              {"ver", ver_of(channel)},
              {"myJiFen", me.score},
              {"myBattleTimes", me.times_left},
              {"fp_need", 0},
              {"point_need", 0},
              {"beginAt", format_cst(g_pool.begin_unix)},
              {"endAt", format_cst(g_pool.end_unix)},
              {"pool", pool_json(g_pool)},
              {"loadout", loadout_json(loadout)},
              {"first", first_flags(loadout)}};
}

void repair_fighter_loadout(const std::string& channel, Fighter& me) {
  Loadout loadout = parse_loadout(me.loadout_json);
  if (loadout.chars.empty()) {
    return;
  }
  // 清掉同名变体 / 重复 id，避免排行与配置页出现两个芙蓉夏蝉。
  {
    std::vector<int> unique;
    std::unordered_set<std::string> nicks;
    for (int id : loadout.chars) {
      if (!in_pool(g_pool.chars, id) || std::find(unique.begin(), unique.end(), id) != unique.end()) {
        continue;
      }
      if (const PoolItem* person = find_item(g_pool.chars, id);
          person && !person->nick.empty() && !nicks.insert(person->nick).second) {
        continue;
      }
      unique.push_back(id);
    }
    loadout.chars = std::move(unique);
  }
  if (loadout.chars.empty()) {
    return;
  }
  const bool kungfu_fixed = ensure_loadout_kungfu(loadout);
  const std::string new_fd = fight_data_of(loadout);
  if (!kungfu_fixed && new_fd == me.fight_data) {
    return;
  }
  me.loadout_json = loadout_json(loadout).dump();
  me.fight_data = new_fd;
  me.fight_power = fight_power_of(loadout);
  save_fighter(channel, me, g_pool.week_id);
}

int repair_human_identities(const std::string& channel) {
  const auto rows = db_query(
      "SELECT account, area, nick, head, fight_data, loadout_json FROM lunjian_fighter WHERE channel=" + q(channel) +
      " AND week_id=" + std::to_string(g_pool.week_id) + " AND is_npc=0");
  int fixed = 0;
  for (const auto& row : rows) {
    Fighter shown;
    shown.account = row.empty() ? "" : row[0];
    shown.area = cell_int(row, 1);
    shown.nick = row.size() > 2 ? row[2] : "";
    shown.head = cell_int(row, 3);
    shown.fight_data = row.size() > 4 ? row[4] : "";
    shown.loadout_json = row.size() > 5 ? row[5] : "";
    const std::string old_nick = shown.nick;
    const int old_head = shown.head;
    apply_display_identity(shown);
    const bool nick_fixed =
        nick_is_placeholder(old_nick, shown.account) && !nick_is_placeholder(shown.nick, shown.account);
    const bool head_fixed = old_head <= 0 && shown.head > 0;
    if (!nick_fixed && !head_fixed) {
      continue;
    }
    persist_identity(channel, shown, old_nick, old_head);
    ++fixed;
  }
  return fixed;
}

json handle_top(const json& req, const std::string& channel) {
  const std::string account = json_str(req, "acc");
  const int area = std::max(1, json_int(req, "area", 1));
  if (account.empty() || area > 66) {
    return fail("账号或存档编号无效");
  }
  ensure_npcs(channel);
  Fighter me = ensure_player(channel, account, area);
  repair_fighter_loadout(channel, me);
  json response = base_board(me, channel);
  const std::string client_ver = json_str(req, "topVer");
  if (!client_ver.empty() && client_ver != "none" && client_ver == response["ver"]) {
    return response;
  }
  const auto rows = db_query(
      "SELECT account, area, nick, head, fight_power, score, fight_data, is_npc FROM lunjian_fighter WHERE channel=" +
      q(channel) + " AND week_id=" + std::to_string(g_pool.week_id) +
      " AND (score>0 OR is_npc=1) ORDER BY score DESC, fight_power DESC LIMIT 50");
  json results = json::array();
  for (const auto& row : rows) {
    Fighter shown;
    shown.account = row.size() > 0 ? row[0] : "";
    shown.area = cell_int(row, 1);
    shown.nick = row.size() > 2 ? row[2] : "";
    shown.head = cell_int(row, 3);
    shown.fight_data = row.size() > 6 ? row[6] : "";
    shown.is_npc = cell_int(row, 7) != 0;
    const std::string old_nick = shown.nick;
    const int old_head = shown.head;
    apply_display_identity(shown);
    persist_identity(channel, shown, old_nick, old_head);
    results.push_back(json{{"account", shown.account},
                           {"acc", shown.account},
                           {"saveIndex", cell_int(row, 1)},
                           {"area", cell_int(row, 1)},
                           {"nick", shown.nick},
                           {"head", shown.head},
                           {"fightPower", cell_int(row, 4)},
                           {"lunJian", cell_int(row, 5)},
                           {"fightData", current_fight_data(shown.fight_data)}});
  }
  response["results"] = results;
  return response;
}

json handle_pool(const json& req, const std::string& channel) {
  const std::string account = json_str(req, "acc");
  const int area = std::max(1, json_int(req, "area", 1));
  if (account.empty() || area > 66) {
    return fail("账号或存档编号无效");
  }
  ensure_npcs(channel);
  Fighter me = ensure_player(channel, account, area);
  repair_fighter_loadout(channel, me);
  const Loadout loadout = parse_loadout(me.loadout_json);
  return json{{"code", 0},
              {"msg", ""},
              {"pool", pool_json(g_pool)},
              {"loadout", loadout_json(loadout)},
              {"first", first_flags(loadout)},
              {"fightPower", me.fight_power}};
}

json handle_set(const json& req, const std::string& channel) {
  const std::string account = json_str(req, "acc");
  const int area = std::max(1, json_int(req, "area", 1));
  const std::string kind = json_str(req, "kind");
  if (account.empty() || area > 66) {
    return fail("账号或存档编号无效");
  }
  Fighter me = ensure_player(channel, account, area);
  Loadout loadout = parse_loadout(me.loadout_json);
  auto require_char = [&](int char_id) {
    return std::find(loadout.chars.begin(), loadout.chars.end(), char_id) != loadout.chars.end();
  };
  if (kind == "char") {
    if (!req.contains("ids") || !req["ids"].is_array() || req["ids"].empty() || req["ids"].size() > 5) {
      return fail("请从本期人物池选择1到5人");
    }
    std::vector<int> ids;
    std::unordered_set<std::string> used_nicks;
    for (const auto& idv : req["ids"]) {
      const int id = idv.is_number_integer() ? idv.get<int>() : 0;
      if (!in_pool(g_pool.chars, id) || std::find(ids.begin(), ids.end(), id) != ids.end()) {
        return fail("只能选择本期论剑池中的人物");
      }
      if (const PoolItem* person = find_item(g_pool.chars, id);
          person && !person->nick.empty() && !used_nicks.insert(person->nick).second) {
        return fail("同名人物只能选择一个");
      }
      ids.push_back(id);
    }
    loadout.chars = ids;
    for (auto it = loadout.skill.begin(); it != loadout.skill.end();) {
      it = require_char(it->first) ? std::next(it) : loadout.skill.erase(it);
    }
    for (auto it = loadout.nei.begin(); it != loadout.nei.end();) {
      it = require_char(it->first) ? std::next(it) : loadout.nei.erase(it);
    }
    for (auto it = loadout.equip.begin(); it != loadout.equip.end();) {
      it = require_char(it->first) ? std::next(it) : loadout.equip.erase(it);
    }
  } else if (kind == "skill" || kind == "nei") {
    const int char_id = json_int(req, "charId");
    const int id = json_int(req, "id");
    if (!require_char(char_id)) {
      return fail("请先选择论剑人物");
    }
    const auto& table = kind == "skill" ? g_pool.skills : g_pool.neis;
    if (id != 0 && !in_pool(table, id)) {
      return fail(kind == "skill" ? "只能选择本期论剑池中的武功" : "只能选择本期论剑池中的内功");
    }
    auto& dest = kind == "skill" ? loadout.skill : loadout.nei;
    if (!try_assign_kungfu(dest, char_id, id)) {
      return fail(kind == "skill" ? "该武功已被其他人选用" : "该内功已被其他人选用");
    }
  } else if (kind == "equip") {
    const int char_id = json_int(req, "charId");
    if (!require_char(char_id)) {
      return fail("请先选择论剑人物");
    }
    if (!req.contains("ids") || !req["ids"].is_array() || req["ids"].size() > 4) {
      return fail("装备最多选择4件");
    }
    std::vector<int> ids;
    for (const auto& idv : req["ids"]) {
      const int id = idv.is_number_integer() ? idv.get<int>() : 0;
      if (id == 0) {
        continue;
      }
      if (!in_pool(g_pool.equips, id) || std::find(ids.begin(), ids.end(), id) != ids.end()) {
        return fail("只能选择本期论剑池中的装备");
      }
      ids.push_back(id);
    }
    if (ids.empty()) {
      loadout.equip.erase(char_id);
    } else {
      loadout.equip[char_id] = ids;
    }
  } else {
    return fail("未知的论剑选择");
  }

  ensure_loadout_kungfu(loadout);
  me.loadout_json = loadout_json(loadout).dump();
  me.fight_data = loadout.chars.empty() ? "" : fight_data_of(loadout);
  me.fight_power = fight_power_of(loadout);
  me.utime = now_unix();
  // 排行榜展示用主存档主角 nick/head，不随论剑池人物切换改写。
  save_fighter(channel, me, g_pool.week_id);
  return json{{"code", 0},
              {"msg", ""},
              {"loadout", loadout_json(loadout)},
              {"first", first_flags(loadout)},
              {"fightPower", me.fight_power},
              {"head", me.head},
              {"nick", me.nick}};
}

json handle_upload(const json& req, const std::string& channel) {
  const std::string account = json_str(req, "acc");
  const int area = std::max(1, json_int(req, "area", 1));
  if (account.empty() || area > 66) {
    return json{{"code", 0}, {"msg", ""}};
  }
  Fighter me = ensure_player(channel, account, area);
  bool updated = false;
  const std::string nick = trim_copy(json_str(req, "nick"));
  if (!nick.empty() && nick != account) {
    me.nick = nick;
    updated = true;
  }
  const int head = json_int(req, "head", 0);
  if (head > 0) {
    me.head = head;
    updated = true;
  }
  const std::string nick_before = me.nick;
  const int head_before = me.head;
  apply_display_identity(me);
  if (me.nick != nick_before || me.head != head_before) {
    updated = true;
  }
  // 客户端 uploadFightPower 会带主存档 fightData/fightPower，一律忽略；
  // 战力只按本期论剑 loadout 重算，避免排行榜被主存档战力污染。
  Loadout loadout = parse_loadout(me.loadout_json);
  if (!loadout.chars.empty()) {
    ensure_loadout_kungfu(loadout);
    me.loadout_json = loadout_json(loadout).dump();
    me.fight_data = fight_data_of(loadout);
    me.fight_power = fight_power_of(loadout);
    updated = true;
  }
  if (updated) {
    me.utime = now_unix();
    save_fighter(channel, me, g_pool.week_id);
    bump_ver(channel);
  }
  return json{{"code", 0}, {"msg", ""}, {"fightPower", me.fight_power}};
}

// 积分榜名次：分数高靠前，同分比战力。返回 1 起。
int fighter_rank(const std::string& channel, int score, int fight_power) {
  const auto rows = db_query(
      "SELECT COUNT(*) FROM lunjian_fighter WHERE channel=" + q(channel) + " AND week_id=" +
      std::to_string(g_pool.week_id) + " AND (score>" + std::to_string(score) + " OR (score=" +
      std::to_string(score) + " AND fight_power>" + std::to_string(fight_power) + "))");
  return 1 + (rows.empty() ? 0 : cell_int(rows[0], 0));
}

// 排名差决定加分：打赢/打输更高名次得多分。
// 赢 8~10，输 2~4。我名次更差（数字更大）说明对手更强 → 靠近上限。
int score_delta_by_rank(bool win, int my_rank, int enemy_rank) {
  const int diff = my_rank - enemy_rank;
  const double t = 0.5 + 0.5 * std::tanh(static_cast<double>(diff) / 8.0);
  if (win) {
    return static_cast<int>(std::lround(8.0 + 2.0 * t));
  }
  return static_cast<int>(std::lround(2.0 + 2.0 * t));
}

json handle_find(const json& req, const std::string& channel) {
  const std::string account = json_str(req, "acc");
  const int area = std::max(1, json_int(req, "area", 1));
  if (account.empty() || area > 66) {
    std::cerr << "[lunjian] find reject bad acc/area acc=" << account << " area=" << area << std::endl;
    return fail("账号或存档编号无效");
  }
  ensure_npcs(channel);
  Fighter me = ensure_player(channel, account, area);
  repair_fighter_loadout(channel, me);
  // loadout 在但 fight_data 丢了：补重建，避免战斗结束后重开误报“未选阵容”。
  if (me.fight_data.empty()) {
    const Loadout loadout = parse_loadout(me.loadout_json);
    if (!loadout.chars.empty()) {
      me.fight_data = fight_data_of(loadout);
      me.fight_power = fight_power_of(loadout);
      me.utime = now_unix();
      save_fighter(channel, me, g_pool.week_id);
      std::cerr << "[lunjian] find rebuilt fightData acc=" << account << " area=" << area
                << " fd=" << me.fight_data.size() << std::endl;
    }
  }
  if (me.fight_data.empty()) {
    std::cerr << "[lunjian] find reject empty loadout acc=" << account << " area=" << area
              << " loadout_len=" << me.loadout_json.size() << std::endl;
    return fail("请先从本期论剑池选择人物、武功和装备");
  }
  if (me.times_left <= 0) {
    return fail("今日论剑次数已用完");
  }

  std::vector<Fighter> chosen_window;
  for (int attempt = 0; attempt < 5; ++attempt) {
    const int span = std::max(12, me.score * (1 << attempt) / 10);
    const auto rows =
        db_query("SELECT id, account, area, nick, head, fight_power, score, is_npc FROM lunjian_fighter WHERE channel=" +
                 q(channel) + " AND week_id=" + std::to_string(g_pool.week_id) + " AND score BETWEEN " +
                 std::to_string(me.score - span) + " AND " + std::to_string(me.score + span) +
                 " AND fight_data IS NOT NULL AND CHAR_LENGTH(fight_data)>2 AND NOT (account=" + q(account) +
                 " AND area=" + std::to_string(area) + ") LIMIT 40");
    std::vector<Fighter> humans;
    std::vector<Fighter> npcs;
    for (const auto& row : rows) {
      Fighter enemy;
      enemy.found = true;
      enemy.account = row.size() > 1 ? row[1] : "";
      enemy.area = cell_int(row, 2);
      enemy.nick = row.size() > 3 ? row[3] : "";
      enemy.head = cell_int(row, 4);
      enemy.fight_power = cell_int(row, 5);
      enemy.score = cell_int(row, 6);
      enemy.is_npc = cell_int(row, 7) != 0;
      (enemy.is_npc ? npcs : humans).push_back(std::move(enemy));
    }
    if (humans.empty() && npcs.empty()) {
      continue;
    }
    std::mt19937 rng(static_cast<unsigned>(now_unix()) ^ static_cast<unsigned>(attempt * 97 + me.score));
    std::shuffle(humans.begin(), humans.end(), rng);
    std::shuffle(npcs.begin(), npcs.end(), rng);
    chosen_window.clear();
    if (!humans.empty() && !npcs.empty()) {
      chosen_window.push_back((rng() % 2) == 0 ? humans[0] : npcs[0]);
    } else if (!humans.empty()) {
      chosen_window.push_back(humans[0]);
    } else {
      chosen_window.push_back(npcs[0]);
    }
    break;
  }
  if (chosen_window.empty()) {
    const auto rows = db_query(
        "SELECT account, area, nick, head, fight_power, score, is_npc FROM lunjian_fighter WHERE channel=" + q(channel) +
        " AND week_id=" + std::to_string(g_pool.week_id) +
        " AND is_npc=1 AND fight_data IS NOT NULL ORDER BY score LIMIT 1");
    if (rows.empty()) {
      std::cerr << "[lunjian] find: no enemy for acc=" << account << " area=" << area
                << " score=" << me.score << std::endl;
      return fail("暂时没有可挑战的对手");
    }
    Fighter enemy;
    enemy.account = rows[0][0];
    enemy.area = cell_int(rows[0], 1);
    enemy.nick = rows[0][2];
    enemy.head = cell_int(rows[0], 3);
    enemy.fight_power = cell_int(rows[0], 4);
    enemy.score = cell_int(rows[0], 5);
    enemy.is_npc = true;
    chosen_window.push_back(std::move(enemy));
  }

  Fighter enemy = chosen_window[0];
  const auto full = db_query("SELECT fight_data FROM lunjian_fighter WHERE channel=" + q(channel) +
                             " AND account=" + q(enemy.account) + " AND area=" + std::to_string(enemy.area) +
                             " AND week_id=" + std::to_string(g_pool.week_id) + " LIMIT 1");
  {
    const std::string raw = full.empty() ? "" : full[0][0];
    enemy.fight_data = current_fight_data(raw);
    if (!enemy.fight_data.empty() && enemy.fight_data != raw) {
      db_exec("UPDATE lunjian_fighter SET fight_data=" + q(enemy.fight_data) + " WHERE channel=" +
              q(channel) + " AND account=" + q(enemy.account) + " AND area=" +
              std::to_string(enemy.area) + " AND week_id=" + std::to_string(g_pool.week_id));
    }
  }
  if (enemy.fight_data.empty()) {
    std::cerr << "[lunjian] find: empty fightData enemy=" << enemy.account << "/" << enemy.area << std::endl;
    return fail("对手阵容数据无效，请再试一次");
  }
  apply_display_identity(enemy);
  // 匹配成功即扣一次；客户端 onTiaoZhanCompleted 用 results 首元素读 fightData/nick。
  me.times_left = std::max(0, me.times_left - 1);
  db_exec("UPDATE lunjian_fighter SET times_left=" + std::to_string(me.times_left) +
          ", times_day=" + std::to_string(me.times_day) + " WHERE channel=" + q(channel) +
          " AND account=" + q(account) + " AND area=" + std::to_string(area) +
          " AND week_id=" + std::to_string(g_pool.week_id));
  const json row = json{{"account", enemy.account},
                        {"acc", enemy.account},
                        {"nick", enemy.nick},
                        {"head", enemy.head},
                        {"area", enemy.area},
                        {"saveIndex", enemy.area},
                        {"fightPower", enemy.fight_power},
                        {"lunJian", enemy.score},
                        {"fightData", enemy.fight_data}};
  std::cerr << "[lunjian] find ok acc=" << account << " area=" << area << " vs=" << enemy.account
            << " fd=" << enemy.fight_data.size() << " times=" << me.times_left << std::endl;
  return json{{"code", 0}, {"myBattleTimes", me.times_left}, {"results", json::array({row})}};
}

json handle_end(const json& req, const std::string& channel) {
  const std::string account = json_str(req, "acc");
  const int area = std::max(1, json_int(req, "area", 1));
  if (account.empty() || area > 66) {
    return fail("账号或存档编号无效");
  }
  Fighter me = ensure_player(channel, account, area);
  const std::string enemy_acc = json_str(req, "eneAcc");
  const int enemy_area = json_int(req, "eneIndex", 1);
  int enemy_power = me.fight_power;
  int enemy_score = me.score;
  std::string enemy_nick = json_str(req, "enemy");
  if (!enemy_acc.empty()) {
    const auto rows = db_query("SELECT fight_power, nick, score FROM lunjian_fighter WHERE channel=" + q(channel) +
                               " AND account=" + q(enemy_acc) + " AND area=" + std::to_string(enemy_area) +
                               " AND week_id=" + std::to_string(g_pool.week_id) + " LIMIT 1");
    if (!rows.empty()) {
      enemy_power = cell_int(rows[0], 0);
      if (enemy_nick.empty()) {
        enemy_nick = rows[0][1];
      }
      enemy_score = cell_int(rows[0], 2);
    }
  }
  const bool win = json_int(req, "br") == 2;
  const int my_rank = fighter_rank(channel, me.score, me.fight_power);
  const int enemy_rank = fighter_rank(channel, enemy_score, enemy_power);
  const int delta = score_delta_by_rank(win, my_rank, enemy_rank);
  std::string msg;
  static const char* kWin[] = {"%s被你狠狠的暴揍了一顿,你得到了%d积分", "%s看形式不妙,仓皇而逃,积分+%d",
                               "你将%s打的跪地求饶,拿到积分%d", "%s还没反应过来就被你秒杀,积分+%d",
                               "你轻松碾压了%s,收获%d积分"};
  static const char* kLose[] = {"你被%s狠狠教训了一顿,仍获得%d积分", "%s轻松击败了你,你拿到%d积分",
                                "你拼尽全力也不敌%s,积分+%d", "面对%s你束手无策,仍收获%d积分",
                                "%s趁你不备偷袭成功,你获得%d积分"};
  std::mt19937 rng(static_cast<unsigned>(now_unix()));
  if (enemy_nick.empty()) {
    enemy_nick = "对手";
  }
  char buf[160];
  std::snprintf(buf, sizeof(buf), (win ? kWin : kLose)[rng() % 5], enemy_nick.c_str(), delta);
  msg = buf;
  me.score = std::max(0, me.score + delta);
  db_exec("UPDATE lunjian_fighter SET score=" + std::to_string(me.score) +
          " WHERE channel=" + q(channel) + " AND account=" + q(account) + " AND area=" +
          std::to_string(area) + " AND week_id=" + std::to_string(g_pool.week_id));
  bump_ver(channel);
  return json{{"code", 0}, {"msg", msg}, {"myJiFen", me.score}, {"myBattleTimes", me.times_left}};
}

}  // namespace

void init(const std::string& data_dir) {
  std::lock_guard<std::mutex> lock(g_mu);
  try {
    ensure_schema();
    std::string doc_path = data_dir + "/doc.json";
    if (!std::filesystem::exists(doc_path)) {
      doc_path = find_resource_path("data/doc.json");
    }
    g_catalog = load_catalog(doc_path);
    g_pinned_week = load_pinned_week(data_dir);
    g_pool = {};
    refresh_pool();
    g_ready = true;
    g_error.clear();
    std::cerr << "[lunjian] week=" << g_pool.week_id << " pinned=" << g_pinned_week
              << " chars=" << g_pool.chars.size()
              << " equips=" << g_pool.equips.size() << " grade=" << g_pool.equip_grade
              << " skills=" << g_pool.skills.size() << " grade=" << g_pool.skill_grade
              << " nei=" << g_pool.neis.size() << " grade=" << g_pool.nei_grade << "\n";
    const auto channels = db_query("SELECT DISTINCT channel FROM lunjian_fighter");
    for (const auto& row : channels) {
      if (!row.empty() && !row[0].empty()) {
        ensure_npcs(row[0]);
        std::cerr << "[lunjian] npc loadout ready channel=" << row[0] << "\n";
      }
    }
  } catch (const std::exception& ex) {
    g_ready = false;
    g_error = ex.what();
    std::cerr << "[lunjian] init failed: " << ex.what() << "\n";
  }
}

std::optional<json> handle(const std::string& action, const json& req, const std::string& channel) {
  const bool mine = action == "selTopLunJian" || action == "lunJianFindEnemy" || action == "lunJianFightEnd" ||
                    action == "uploadFightPower" || action == "getLunJianPool" || action == "setLunJianLoadout";
  if (!mine) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> lock(g_mu);
  if (!g_ready) {
    if (action == "uploadFightPower") {
      return json{{"code", 0}, {"msg", ""}};
    }
    return fail(g_error.empty() ? "论剑未就绪" : g_error);
  }
  try {
    refresh_pool();
    const std::string use_channel = channel.empty() ? "none" : channel;
    static std::unordered_set<std::string> identity_repaired;
    if (identity_repaired.insert(use_channel).second && repair_human_identities(use_channel) > 0) {
      bump_ver(use_channel);
    }
    if (action == "selTopLunJian") {
      return handle_top(req, use_channel);
    }
    if (action == "lunJianFindEnemy") {
      return handle_find(req, use_channel);
    }
    if (action == "lunJianFightEnd") {
      return handle_end(req, use_channel);
    }
    if (action == "uploadFightPower") {
      return handle_upload(req, use_channel);
    }
    if (action == "getLunJianPool") {
      return handle_pool(req, use_channel);
    }
    return handle_set(req, use_channel);
  } catch (const std::exception& ex) {
    std::cerr << "[lunjian] " << action << " failed: " << ex.what() << "\n";
    return fail("论剑服务暂时不可用");
  }
}

}  // namespace jh::lunjian
