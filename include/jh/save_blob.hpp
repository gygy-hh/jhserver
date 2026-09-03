#pragma once

#include "jh/mail.hpp"

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

// 将待发邮件写入 dat.json.myGift，供客户端邮箱 UI 领取（原版 recvMail 流程）
std::string inject_mygift(const std::string& blob, int save_index, const std::vector<mail::MailRecord>& mails);

}  // namespace jh::save_blob
