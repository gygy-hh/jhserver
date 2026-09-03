#include "jh/save_crypto.hpp"

#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: test_decrypt <save.json> [save_index]\n";
    return 1;
  }
  std::ifstream in(argv[1]);
  nlohmann::json j;
  in >> j;
  const std::string blob = j["save"].get<std::string>();
  const auto pos = blob.find("dat.json");
  if (pos == std::string::npos) {
    std::cerr << "no dat.json\n";
    return 1;
  }
  const auto start = blob.find("____", pos + 8);
  const auto end = blob.find("____", start + 4);
  const std::string cipher = blob.substr(start + 4, end - start - 4);
  const int save_index = argc >= 3 ? std::stoi(argv[2]) : 1;
  for (int idx = 0; idx < 10; ++idx) {
    try {
      jh::save_crypto::DatKeyMode used{};
      const std::string plain = jh::save_crypto::decrypt_dat_auto(cipher, idx, &used);
      std::cout << "OK idx=" << idx << " mode=" << static_cast<int>(used) << " head=" << plain.substr(0, 120) << "\n";
    } catch (...) {
    }
  }
  return 0;
}
