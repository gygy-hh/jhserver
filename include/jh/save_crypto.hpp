#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace jh::save_crypto {

enum class DatKeyMode {
  kLegacyPlain,   // ab1234abab1234ab
  kPaPa,          // getPaPa(save_index)
  kPaPaNew2,      // getPaPaNew2(save_index) — 当前 dat.json 默认
  kOverride,      // data/save_keys.json 中 Frida 抓取的密钥
};

void init(const std::string& data_dir);

std::string derive_dat_key(int save_index, DatKeyMode mode);
std::optional<std::string> get_override_key(int save_index);
void set_override_key(int save_index, const std::string& key16);

std::string encrypt_dat(const std::string& plain, int save_index, DatKeyMode mode = DatKeyMode::kPaPaNew2);
std::string decrypt_dat(const std::string& cipher_b64, int save_index, DatKeyMode mode = DatKeyMode::kPaPaNew2);

// 自动尝试 override / new2 / papa / plain
std::string decrypt_dat_auto(const std::string& cipher_b64, int save_index, DatKeyMode* used = nullptr);

}  // namespace jh::save_crypto
