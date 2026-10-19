#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace jh::lunjian {

struct CatItem {
  int id = 0;
  std::string nick;
  int star = 0;
  int head = 0;
  int type = 0;
  int sex = 1;
  int icon = 0;
  int quality = 0;
};

struct Catalog {
  std::vector<CatItem> chars;
  std::vector<CatItem> equips;
  std::vector<CatItem> skills;
  std::vector<CatItem> neis;
};

struct PoolItem {
  int id = 0;
  std::string nick;
  int star = 0;
  bool graded = false;
  int head = 0;
  int type = 0;
  int sex = 1;
  int icon = 0;
  int quality = 0;
};

struct WeekPool {
  int64_t week_id = 0;
  int64_t begin_unix = 0;
  int64_t end_unix = 0;
  int equip_grade = 0;
  int skill_grade = 0;
  int nei_grade = 0;
  std::vector<PoolItem> chars;
  std::vector<PoolItem> equips;
  std::vector<PoolItem> skills;
  std::vector<PoolItem> neis;
};

// 装备/武功/内功池：底档条数 + 高档（跨一级 featured）上限；三类独立 roll。
constexpr int kPoolPlainCount = 14;
constexpr int kPoolGradedMax = 4;

// 周一 04:00（UTC+8）切换。week_id 是该时刻对应的“平移日”序号。
int64_t week_id_at(int64_t now_unix);
void week_bounds(int64_t week_id, int64_t& begin_unix, int64_t& end_unix);

Catalog load_catalog(const std::string& doc_json_path);
WeekPool build_week_pool(const Catalog& catalog, int64_t week_id);

const PoolItem* find_item(const std::vector<PoolItem>& items, int id);

}  // namespace jh::lunjian
