#include "jh/save_crypto.hpp"

#include "jh/crypto.hpp"

#include <array>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <vector>

namespace jh::save_crypto {

namespace {

using json = nlohmann::json;

std::mutex g_mu;
std::string g_keys_path;
json g_overrides = json::object();

constexpr const char* kLegacyKey = "ab1234abab1234ab";
constexpr const char* kSaltNew2 = "habo6qeegjpqdgfl";
constexpr const char* kSaltPaPa = "bc86tycaxnml5uop";

std::string interleave(char fill, const char* fixed16) {
  std::string out;
  out.reserve(32);
  for (int i = 0; i < 16; ++i) {
    out.push_back(fixed16[i]);
    out.push_back(fill);
  }
  return out;
}

std::string derive_papa_new2(int save_index) {
  const char fill = kSaltNew2[save_index % 16];
  static constexpr char kFixed[] = "vabi98dftdlpzeag";
  const std::string material = interleave(fill, kFixed);
  const std::string md5 = crypto::md5_hex(material);
  const size_t off = static_cast<size_t>(save_index % 18);
  if (off >= md5.size()) {
    throw std::runtime_error("invalid md5 substr for getPaPaNew2");
  }
  return md5.substr(off, std::min<std::size_t>(16, md5.size() - off));
}

std::string derive_papa(int save_index) {
  const char fill = kSaltPaPa[save_index % 16];
  static constexpr char kFixed[] = "acdf0899cklnpxzdh";
  const std::string material = interleave(fill, kFixed);
  const std::string md5 = crypto::md5_hex(material);
  const size_t off = static_cast<size_t>(save_index % 7);
  if (off + 16 > md5.size()) {
    throw std::runtime_error("invalid md5 substr for getPaPa");
  }
  return md5.substr(off, 16);
}

std::string decrypt_with_key(const std::string& cipher_b64, const std::string& key) {
  const auto raw = crypto::base64_decode(cipher_b64);
  const auto plain = crypto::xxtea_decrypt(raw, key);
  return std::string(plain.begin(), plain.end());
}

std::string encrypt_with_key(const std::string& plain, const std::string& key) {
  std::vector<uint8_t> data(plain.begin(), plain.end());
  const auto encrypted = crypto::xxtea_encrypt(data, key);
  return crypto::base64_encode(encrypted);
}

bool looks_like_json(const std::string& text) {
  if (text.empty()) {
    return false;
  }
  const char c = text.front();
  return c == '{' || c == '[';
}

void persist_overrides_unlocked() {
  std::ofstream out(g_keys_path);
  out << g_overrides.dump(2);
}

void load_overrides_unlocked() {
  g_overrides = json{{"by_index", json::object()}};
  std::ifstream in(g_keys_path);
  if (!in) {
    return;
  }
  try {
    in >> g_overrides;
  } catch (...) {
  }
  if (!g_overrides.contains("by_index") || !g_overrides["by_index"].is_object()) {
    g_overrides["by_index"] = json::object();
  }
}

}  // namespace

void init(const std::string& data_dir) {
  std::lock_guard<std::mutex> lock(g_mu);
  g_keys_path = data_dir + "/save_keys.json";
  load_overrides_unlocked();
}

std::optional<std::string> get_override_key(int save_index) {
  std::lock_guard<std::mutex> lock(g_mu);
  const auto key = std::to_string(save_index);
  if (g_overrides.contains("by_index") && g_overrides["by_index"].contains(key)) {
    const std::string val = g_overrides["by_index"][key].get<std::string>();
    if (val.size() >= 15) {
      return val.substr(0, 16);
    }
  }
  return std::nullopt;
}

void set_override_key(int save_index, const std::string& key16) {
  std::lock_guard<std::mutex> lock(g_mu);
  if (key16.size() < 15) {
    throw std::runtime_error("key must be at least 15 characters");
  }
  g_overrides["by_index"][std::to_string(save_index)] = key16.substr(0, 16);
  persist_overrides_unlocked();
}

std::string derive_dat_key(int save_index, DatKeyMode mode) {
  if (mode == DatKeyMode::kOverride) {
    if (auto key = get_override_key(save_index)) {
      return *key;
    }
    throw std::runtime_error("override key missing for save index " + std::to_string(save_index));
  }
  switch (mode) {
    case DatKeyMode::kLegacyPlain:
      return kLegacyKey;
    case DatKeyMode::kPaPa:
      return derive_papa(save_index);
    case DatKeyMode::kPaPaNew2:
      return derive_papa_new2(save_index);
  }
  return derive_papa_new2(save_index);
}

std::string encrypt_dat(const std::string& plain, int save_index, DatKeyMode mode) {
  return encrypt_with_key(plain, derive_dat_key(save_index, mode));
}

std::string decrypt_dat(const std::string& cipher_b64, int save_index, DatKeyMode mode) {
  return decrypt_with_key(cipher_b64, derive_dat_key(save_index, mode));
}

std::string decrypt_dat_auto(const std::string& cipher_b64, int save_index, DatKeyMode* used) {
  if (auto override_key = get_override_key(save_index)) {
    try {
      const std::string plain = decrypt_with_key(cipher_b64, *override_key);
      if (looks_like_json(plain)) {
        if (used) {
          *used = DatKeyMode::kOverride;
        }
        return plain;
      }
    } catch (...) {
    }
  }

  static constexpr DatKeyMode kModes[] = {
      DatKeyMode::kPaPaNew2,
      DatKeyMode::kPaPa,
      DatKeyMode::kLegacyPlain,
  };
  std::string last_err;
  for (DatKeyMode mode : kModes) {
    try {
      const std::string plain = decrypt_dat(cipher_b64, save_index, mode);
      if (looks_like_json(plain)) {
        if (used) {
          *used = mode;
        }
        return plain;
      }
      last_err = "decrypted but not json";
    } catch (const std::exception& ex) {
      last_err = ex.what();
    }
  }
  throw std::runtime_error("dat.json decrypt failed: " + last_err);
}

}  // namespace jh::save_crypto
