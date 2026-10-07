#include "jh/save_blob.hpp"

#include "jh/save_crypto.hpp"

#include <functional>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>

namespace jh::save_blob {

namespace {

using json = nlohmann::json;

std::vector<std::string> split_blob(const std::string& blob) {
  std::vector<std::string> parts;
  std::string cur;
  const std::string delim = "____";
  for (size_t i = 0; i < blob.size();) {
    if (blob.compare(i, delim.size(), delim) == 0) {
      parts.push_back(cur);
      cur.clear();
      i += delim.size();
      continue;
    }
    cur.push_back(blob[i++]);
  }
  parts.push_back(cur);
  return parts;
}

std::vector<SaveSegment> from_parts(const std::vector<std::string>& parts) {
  std::vector<SaveSegment> out;
  size_t i = 0;
  if (!parts.empty() && parts[0].empty()) {
    i = 1;
  }
  for (; i + 1 < parts.size(); i += 2) {
    SaveSegment seg;
    seg.name = parts[i];
    seg.content = parts[i + 1];
    if (!seg.name.empty()) {
      out.push_back(std::move(seg));
    }
  }
  return out;
}

std::vector<std::string> to_parts(const std::vector<SaveSegment>& segments) {
  std::vector<std::string> parts;
  parts.push_back("");
  for (const auto& seg : segments) {
    parts.push_back(seg.name);
    parts.push_back(seg.content);
  }
  return parts;
}

// 解密 dat.json、交给 edit 修改、按原密钥重新加密。edit 返回 false 表示无改动。
std::string edit_dat(const std::string& blob, int save_index, const std::function<bool(json&)>& edit) {
  auto dat_cipher = get_segment(blob, "dat.json");
  if (!dat_cipher) {
    throw std::runtime_error("save blob missing dat.json");
  }

  save_crypto::DatKeyMode mode = save_crypto::DatKeyMode::kPaPaNew2;
  std::string dat_plain;
  if (*dat_cipher == "null" || dat_cipher->empty()) {
    dat_plain = "{}";
  } else {
    dat_plain = save_crypto::decrypt_dat_auto(*dat_cipher, save_index, &mode);
  }

  json doc = json::parse(dat_plain);
  if (!edit(doc)) {
    return blob;
  }

  const std::string encrypted = save_crypto::encrypt_dat(doc.dump(-1, ' ', false, json::error_handler_t::replace),
                                                         save_index, mode);
  return set_segment(blob, "dat.json", encrypted);
}

}  // namespace

std::vector<SaveSegment> parse(const std::string& blob) {
  return from_parts(split_blob(blob));
}

std::string pack(const std::vector<SaveSegment>& segments) {
  const auto parts = to_parts(segments);
  std::ostringstream oss;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) {
      oss << "____";
    }
    oss << parts[i];
  }
  return oss.str();
}

std::optional<std::string> get_segment(const std::string& blob, const std::string& name) {
  for (const auto& seg : parse(blob)) {
    if (seg.name == name) {
      return seg.content;
    }
  }
  return std::nullopt;
}

std::string set_segment(const std::string& blob, const std::string& name, const std::string& content) {
  auto segments = parse(blob);
  bool found = false;
  for (auto& seg : segments) {
    if (seg.name == name) {
      seg.content = content;
      found = true;
      break;
    }
  }
  if (!found) {
    segments.push_back({name, content});
  }
  return pack(segments);
}

std::string strip_bl(const std::string& blob, int save_index) {
  return edit_dat(blob, save_index, [](json& doc) { return doc.is_object() && doc.erase("bl") > 0; });
}

std::string apply_bl(const std::string& blob, int save_index) {
  return edit_dat(blob, save_index, [](json& doc) {
    if (!doc.is_object() || doc.contains("bl")) {
      return false;
    }
    doc["bl"] = 1;
    return true;
  });
}

}  // namespace jh::save_blob
