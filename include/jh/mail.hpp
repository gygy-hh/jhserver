#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace jh::mail {

struct MailRecord {
  std::string id;
  std::string desp;
  std::map<std::string, int> items;
  int area = 0;
  int64_t created_at = 0;
  int64_t push_version = 0;
  // pending: 待推送 | pushed: 已通过 getInitData 实时发放
  // in_save/claimed: 兼容旧版云存档邮件流程
  std::string status;
};

struct BroadcastJob {
  std::string id;
  std::string desp;
  std::map<std::string, int> items;
  int64_t scheduled_at = 0;
  int64_t created_at = 0;
  int64_t completed_at = 0;
  size_t recipient_count = 0;
  size_t sent_count = 0;
  std::string status;
};

void init(const std::string& data_dir);

std::string send(const std::string& acc, int area, const std::string& desp,
                 const std::map<std::string, int>& items);
std::vector<MailRecord> list(const std::string& acc, int area);
bool remove(const std::string& acc, int area, const std::string& mail_id);
void remove_account(const std::string& acc);

std::vector<MailRecord> pending_for_injection(const std::string& acc, int area);
void mark_in_save(const std::string& acc, int area, const std::vector<std::string>& mail_ids);
void mark_pushed(const std::string& acc, int area, const std::vector<std::string>& mail_ids);

// uploadSave 时对比 dat.json.myGift，将已消失的邮件标记为 claimed
void sync_claimed_from_save(const std::string& acc, int area, int save_index, const std::string& save_blob);

size_t pending_count(const std::string& acc, int area);

std::string create_broadcast(const std::string& desp, const std::map<std::string, int>& items,
                             int64_t scheduled_at);
std::vector<BroadcastJob> list_broadcasts();
bool cancel_broadcast(const std::string& job_id);

}  // namespace jh::mail
