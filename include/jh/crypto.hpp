#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace jh::crypto {

constexpr const char* kMttSalt = "17031703";

std::string md5_hex(const std::string& input);
std::string md5_file(const std::string& path);
std::string calc_mtt(uint32_t data_account);

std::string get_zhiling_psw(int app_version);

std::string base64_encode(const std::vector<uint8_t>& data);
std::vector<uint8_t> base64_decode(const std::string& encoded);

std::vector<uint8_t> xxtea_encrypt(const std::vector<uint8_t>& data, const std::string& key);
std::vector<uint8_t> xxtea_decrypt(const std::vector<uint8_t>& data, const std::string& key);

std::string encrypt_payload(const std::string& plain_json, int app_version);
std::string decrypt_payload(const std::string& cipher_b64, int app_version);

bool is_encrypted_body_action(const std::string& action);
bool is_encrypted_response_action(const std::string& action);

}  // namespace jh::crypto
