#include "jh/lunjian_pool.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <random>
#include <stdexcept>
#include <unordered_set>

namespace jh::lunjian {

namespace {

constexpr int64_t kShiftSec = 4 * 60 * 60;
constexpr int64_t kWeekSec = 7 * 24 * 60 * 60;
constexpr int kCharCount = 12;
constexpr int kFixedCharIds[] = {912, 201, 203, 91203, 202, 208, 283, 282, 280, 286, 285, 284};
// 2026-10-19 04:00 至 2026-10-26 04:00。这一期武功、内功、装备按指定名单，不随机。
constexpr int64_t kFirstPeriodWeekId = 20745;
constexpr int kFirstPeriodSkillIds[] = {
    3218, 3212, 3207, 3126, 202607176, 3134, 3013, 3029, 3015, 913103, 3214, 3204,
    3213, 20260102, 3122, 3118, 202606191, 202605221, 202606064, 3024, 3028, 3203, 3103, 3003};
constexpr int kFirstPeriodNeiIds[] = {3851, 3829, 3835, 202606054, 3808, 1157,
                                      20260766, 202606196, 203801, 203800, 3000005, 3810};
constexpr int kFirstPeriodEquipIds[] = {20265226, 20265312, 5118,     194002, 5310, 5820, 204006, 204004, 5413, 20265209,
                                        204003,   20266202, 5819,     20265222, 204007, 5215, 20265213, 20265221, 5848,
                                        5218};

int min_star_of(const std::vector<CatItem>& items) {
  int star = 99;
  for (const auto& item : items) {
    star = std::min(star, item.star);
  }
  return items.empty() ? 0 : star;
}

int count_star(const std::vector<CatItem>& items, int star) {
  int count = 0;
  for (const auto& item : items) {
    if (item.star == star) {
      ++count;
    }
  }
  return count;
}

int roll_grade(const std::vector<CatItem>& items, std::mt19937& rng) {
  const int low = min_star_of(items);
  std::vector<int> options;
  for (int star = 1; star <= 6; ++star) {
    if (star != low && count_star(items, star) > 0) {
      options.push_back(star);
    }
  }
  if (options.empty()) {
    return 0;
  }
  return options[rng() % options.size()];
}

PoolItem to_pool(const CatItem& item, bool graded) {
  PoolItem out;
  out.id = item.id;
  out.nick = item.nick;
  out.star = item.star;
  out.graded = graded;
  out.head = item.head;
  out.type = item.type;
  out.sex = item.sex;
  out.icon = item.icon;
  out.quality = item.quality;
  return out;
}

std::vector<PoolItem> pick_category(const std::vector<CatItem>& items, std::mt19937& rng) {
  const int low = min_star_of(items);
  const int grade = roll_grade(items, rng);
  int graded_n = grade == 0 ? 0 : 1 + static_cast<int>(rng() % kPoolGradedMax);

  std::vector<CatItem> featured;
  std::vector<CatItem> plain;
  for (const auto& item : items) {
    if (grade > 0 && item.star == grade) {
      featured.push_back(item);
    } else if (item.star == low) {
      plain.push_back(item);
    }
  }
  std::shuffle(featured.begin(), featured.end(), rng);
  std::shuffle(plain.begin(), plain.end(), rng);

  graded_n = std::min(graded_n, static_cast<int>(featured.size()));
  std::vector<PoolItem> out;
  out.reserve(static_cast<size_t>(graded_n + kPoolPlainCount));
  for (int i = 0; i < graded_n; ++i) {
    out.push_back(to_pool(featured[static_cast<size_t>(i)], true));
  }
  const int plain_n = std::min(kPoolPlainCount, static_cast<int>(plain.size()));
  for (int i = 0; i < plain_n; ++i) {
    out.push_back(to_pool(plain[static_cast<size_t>(i)], false));
  }
  std::shuffle(out.begin(), out.end(), rng);
  return out;
}

std::vector<PoolItem> pick_ids(const std::vector<CatItem>& items, const int* ids, int count) {
  std::vector<PoolItem> out;
  out.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    const CatItem* found = nullptr;
    for (const auto& item : items) {
      if (item.id == ids[i]) {
        found = &item;
        break;
      }
    }
    if (!found) {
      throw std::runtime_error("lunjian first period id missing " + std::to_string(ids[i]));
    }
    out.push_back(to_pool(*found, false));
  }
  return out;
}

std::vector<PoolItem> pick_chars(const std::vector<CatItem>& items, std::mt19937& rng) {
  std::unordered_set<std::string> used_nicks;
  std::unordered_set<int> used_ids;
  std::vector<PoolItem> out;
  out.reserve(static_cast<size_t>(kCharCount));
  auto try_add = [&](const CatItem& item) {
    if (item.id <= 0 || used_ids.count(item.id) || item.nick.empty() || used_nicks.count(item.nick)) {
      return false;
    }
    used_ids.insert(item.id);
    used_nicks.insert(item.nick);
    out.push_back(to_pool(item, false));
    return true;
  };
  // 固定阵容优先；同名变体（如芙蓉夏蝉 912/908）只保留先命中的一个。
  for (int id : kFixedCharIds) {
    if (static_cast<int>(out.size()) >= kCharCount) {
      break;
    }
    for (const auto& item : items) {
      if (item.id == id) {
        try_add(item);
        break;
      }
    }
  }
  if (static_cast<int>(out.size()) >= kCharCount) {
    return out;
  }
  // 缺员时用其它可战人物补齐，仍保证 id/昵称唯一，绝不回退成全表乱抽导致同名重复。
  std::vector<CatItem> copy;
  copy.reserve(items.size());
  for (const auto& item : items) {
    if (item.id > 0 && !item.nick.empty() && !used_ids.count(item.id) && !used_nicks.count(item.nick)) {
      copy.push_back(item);
    }
  }
  std::shuffle(copy.begin(), copy.end(), rng);
  for (const auto& item : copy) {
    if (static_cast<int>(out.size()) >= kCharCount) {
      break;
    }
    try_add(item);
  }
  return out;
}

int featured_grade(const std::vector<PoolItem>& items) {
  for (const auto& item : items) {
    if (item.graded) {
      return item.star;
    }
  }
  return 0;
}

int read_int(const nlohmann::json& value, const char* key, int fallback) {
  const auto it = value.find(key);
  if (it == value.end() || it->is_null()) {
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

CatItem read_item(const nlohmann::json& value, int fallback_id) {
  CatItem item;
  item.id = read_int(value, "id", fallback_id);
  if (value.contains("nick") && value["nick"].is_string()) {
    item.nick = value["nick"].get<std::string>();
  }
  item.quality = read_int(value, "quality", 0);
  item.star = read_int(value, "star", item.quality);
  item.head = read_int(value, "head", item.id);
  item.type = read_int(value, "type", 0);
  item.sex = read_int(value, "sex", 1);
  item.icon = read_int(value, "icon", item.id);
  if (item.nick.empty()) {
    item.nick = std::to_string(item.id);
  }
  return item;
}

void read_map(const nlohmann::json& root, const char* key, std::vector<CatItem>& out, bool chars) {
  const auto it = root.find(key);
  if (it == root.end() || !it->is_object()) {
    throw std::runtime_error(std::string("doc.json missing ") + key);
  }
  out.reserve(it->size());
  for (auto entry = it->begin(); entry != it->end(); ++entry) {
    if (!entry.value().is_object()) {
      continue;
    }
    int fallback = 0;
    try {
      fallback = std::stoi(entry.key());
    } catch (...) {
    }
    CatItem item = read_item(entry.value(), fallback);
    if (item.id <= 0) {
      continue;
    }
    // 本期指定角色里有 group 不为 1 的人物，人物表全部保留，再按 id 取用。
    (void)chars;
    out.push_back(std::move(item));
  }
  if (out.empty()) {
    throw std::runtime_error(std::string("doc.json has no usable ") + key);
  }
}

}  // namespace

int64_t week_id_at(int64_t now_unix) {
  const int64_t shifted_day = (now_unix + kShiftSec) / 86400;
  const int dow = static_cast<int>((shifted_day + 3) % 7);
  return shifted_day - dow;
}

void week_bounds(int64_t week_id, int64_t& begin_unix, int64_t& end_unix) {
  begin_unix = week_id * 86400 - kShiftSec;
  end_unix = begin_unix + kWeekSec;
}

Catalog load_catalog(const std::string& doc_json_path) {
  std::ifstream in(doc_json_path);
  if (!in) {
    throw std::runtime_error("cannot open " + doc_json_path);
  }
  nlohmann::json root;
  in >> root;
  Catalog catalog;
  read_map(root, "person", catalog.chars, true);
  read_map(root, "equip", catalog.equips, false);
  read_map(root, "skill", catalog.skills, false);
  read_map(root, "neiGong", catalog.neis, false);
  return catalog;
}

WeekPool build_week_pool(const Catalog& catalog, int64_t week_id) {
  const auto seed = static_cast<std::mt19937::result_type>(static_cast<uint64_t>(week_id) * 1315423911u);
  std::mt19937 char_rng(seed ^ 0xC4A11u);
  std::mt19937 equip_rng(seed ^ 0xE011Au);
  std::mt19937 skill_rng(seed ^ 0x51111u);
  std::mt19937 nei_rng(seed ^ 0x4E31Au);

  WeekPool pool;
  pool.week_id = week_id;
  week_bounds(week_id, pool.begin_unix, pool.end_unix);
  pool.chars = pick_chars(catalog.chars, char_rng);
  if (week_id == kFirstPeriodWeekId) {
    pool.equips = pick_ids(catalog.equips, kFirstPeriodEquipIds,
                           static_cast<int>(sizeof(kFirstPeriodEquipIds) / sizeof(kFirstPeriodEquipIds[0])));
    pool.skills = pick_ids(catalog.skills, kFirstPeriodSkillIds,
                           static_cast<int>(sizeof(kFirstPeriodSkillIds) / sizeof(kFirstPeriodSkillIds[0])));
    pool.neis = pick_ids(catalog.neis, kFirstPeriodNeiIds,
                         static_cast<int>(sizeof(kFirstPeriodNeiIds) / sizeof(kFirstPeriodNeiIds[0])));
  } else {
    pool.equips = pick_category(catalog.equips, equip_rng);
    pool.skills = pick_category(catalog.skills, skill_rng);
    pool.neis = pick_category(catalog.neis, nei_rng);
  }
  pool.equip_grade = featured_grade(pool.equips);
  pool.skill_grade = featured_grade(pool.skills);
  pool.nei_grade = featured_grade(pool.neis);
  return pool;
}

const PoolItem* find_item(const std::vector<PoolItem>& items, int id) {
  for (const auto& item : items) {
    if (item.id == id) {
      return &item;
    }
  }
  return nullptr;
}

}  // namespace jh::lunjian
