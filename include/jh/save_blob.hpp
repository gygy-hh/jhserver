#pragma once

#include <optional>
#include <string>
#include <vector>

namespace jh::save_blob {

struct SaveSegment {
  std::string name;
  std::string content;
};

std::vector<SaveSegment> parse(const std::string& blob);
std::string pack(const std::vector<SaveSegment>& segments);

std::optional<std::string> get_segment(const std::string& blob, const std::string& name);
std::string set_segment(const std::string& blob, const std::string& name, const std::string& content);

// 删除 dat.json 的 bl 字段。客户端 JhPerson::getPower 在 isBl() 为真时把怪物伤害乘以 10000
std::string strip_bl(const std::string& blob, int save_index);

// 写入 dat.json 的 bl=1，使客户端 isBl() 为真并触发 10000 倍伤害镇压
std::string apply_bl(const std::string& blob, int save_index);

}  // namespace jh::save_blob
