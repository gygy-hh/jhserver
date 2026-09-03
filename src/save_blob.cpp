#include "jh/save_blob.hpp"

#include "jh/save_crypto.hpp"

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

json build_mygift_entry(const mail::MailRecord& mail) {
  json entry{{"desp", mail.desp}};
  for (const auto& [prop_id, count] : mail.items) {
    entry[prop_id] = count;
  }
  return entry;
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

std::string inject_mygift(const std::string& blob, int save_index, const std::vector<mail::MailRecord>& mails) {
  if (mails.empty()) {
    return blob;
  }

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
  if (!doc.contains("myGift") || !doc["myGift"].is_object()) {
    doc["myGift"] = json::object();
  }
  for (const auto& mail : mails) {
    doc["myGift"][mail.id] = build_mygift_entry(mail);
  }

  const std::string encrypted = save_crypto::encrypt_dat(doc.dump(-1, ' ', false, json::error_handler_t::replace),
                                                         save_index, mode);
  return set_segment(blob, "dat.json", encrypted);
}

}  // namespace jh::save_blob
