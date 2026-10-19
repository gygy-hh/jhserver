#include "jh/lunjian_pool.hpp"

#include <iostream>
#include <set>
#include <string>

namespace {

int fail(const std::string& msg) {
  std::cerr << msg << "\n";
  return 1;
}

jh::lunjian::CatItem item(int id, int star) {
  jh::lunjian::CatItem out;
  out.id = id;
  out.nick = "n" + std::to_string(id);
  out.star = star;
  out.head = id;
  out.icon = id;
  return out;
}

void add_band(std::vector<jh::lunjian::CatItem>& dest, int start, int count, int star) {
  for (int i = 0; i < count; ++i) {
    dest.push_back(item(start + i, star));
  }
}

int graded_count(const std::vector<jh::lunjian::PoolItem>& items) {
  int count = 0;
  for (const auto& entry : items) {
    if (entry.graded) {
      ++count;
    }
  }
  return count;
}

bool same_pool(const jh::lunjian::WeekPool& a, const jh::lunjian::WeekPool& b) {
  auto ids = [](const std::vector<jh::lunjian::PoolItem>& items) {
    std::string out;
    for (const auto& entry : items) {
      out += std::to_string(entry.id) + ":" + std::to_string(entry.star) + ":" + (entry.graded ? "1" : "0") + ",";
    }
    return out;
  };
  return ids(a.chars) == ids(b.chars) && ids(a.equips) == ids(b.equips) && ids(a.skills) == ids(b.skills) &&
         ids(a.neis) == ids(b.neis);
}

bool check_category(const std::vector<jh::lunjian::PoolItem>& items, int low, int grade) {
  if (graded_count(items) > jh::lunjian::kPoolGradedMax) {
    return false;
  }
  std::set<int> ids;
  for (const auto& entry : items) {
    if (!ids.insert(entry.id).second) {
      return false;
    }
    if (entry.graded) {
      if (entry.star != grade || grade == 0) {
        return false;
      }
    } else if (entry.star != low) {
      return false;
    }
  }
  return true;
}

}  // namespace

int main() {
  using namespace jh::lunjian;
  int64_t begin = 0;
  int64_t end = 0;
  const int64_t sample = 1760000000;
  const int64_t week = week_id_at(sample);
  week_bounds(week, begin, end);
  if (week_id_at(begin) != week || week_id_at(end - 1) != week || week_id_at(end) == week || end - begin != 7 * 86400) {
    return fail("week bounds are not Monday 04:00 CST");
  }

  Catalog catalog;
  add_band(catalog.chars, 1000, 20, 1);
  add_band(catalog.equips, 2000, 12, 1);
  add_band(catalog.equips, 2100, 6, 3);
  add_band(catalog.equips, 2200, 6, 5);
  add_band(catalog.skills, 3000, 10, 0);
  add_band(catalog.skills, 3100, 6, 2);
  add_band(catalog.skills, 3200, 6, 6);
  add_band(catalog.neis, 4000, 8, 0);
  add_band(catalog.neis, 4100, 6, 4);
  add_band(catalog.neis, 4200, 6, 6);

  int equip_graded = 0;
  int skill_graded = 0;
  int nei_graded = 0;
  bool grades_differ = false;
  for (int64_t i = 0; i < 40; ++i) {
    const WeekPool pool = build_week_pool(catalog, week + i * 7);
    const WeekPool again = build_week_pool(catalog, week + i * 7);
    if (!same_pool(pool, again)) {
      return fail("pool is not stable for a week");
    }
    if (pool.chars.size() != 12 || !check_category(pool.equips, 1, pool.equip_grade) ||
        !check_category(pool.skills, 0, pool.skill_grade) || !check_category(pool.neis, 0, pool.nei_grade)) {
      return fail("pool broke the grade cap");
    }
    equip_graded += graded_count(pool.equips);
    skill_graded += graded_count(pool.skills);
    nei_graded += graded_count(pool.neis);
    if (pool.equip_grade != pool.skill_grade || pool.skill_grade != pool.nei_grade) {
      grades_differ = true;
    }
  }
  if (equip_graded == 0 || skill_graded == 0 || nei_graded == 0 || !grades_differ) {
    return fail("category grades were not rolled independently");
  }
  std::cout << "lunjian pool ok\n";
  return 0;
}
