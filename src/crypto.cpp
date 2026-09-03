#include "jh/crypto.hpp"

#include <array>
#include <cstring>
#include <stdexcept>

namespace jh::crypto {

namespace {

constexpr uint32_t kDelta = 0x9E3779B9u;

// RFC 1321 MD5
struct Md5Ctx {
  uint32_t state[4];
  uint32_t count[2];
  uint8_t buffer[64];
};

inline uint32_t f(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | (~x & z); }
inline uint32_t g(uint32_t x, uint32_t y, uint32_t z) { return (x & z) | (y & ~z); }
inline uint32_t h(uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; }
inline uint32_t i(uint32_t x, uint32_t y, uint32_t z) { return y ^ (x | ~z); }
inline uint32_t rotl(uint32_t x, uint32_t n) { return (x << n) | (x >> (32 - n)); }

void md5_transform(uint32_t state[4], const uint8_t block[64]) {
  uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  uint32_t x[16];
  for (int j = 0; j < 16; ++j) {
    x[j] = static_cast<uint32_t>(block[j * 4]) | (static_cast<uint32_t>(block[j * 4 + 1]) << 8) |
           (static_cast<uint32_t>(block[j * 4 + 2]) << 16) | (static_cast<uint32_t>(block[j * 4 + 3]) << 24);
  }

  auto step = [&](auto ff, uint32_t& aa, uint32_t bb, uint32_t cc, uint32_t dd, uint32_t kk, uint32_t s, uint32_t tt) {
    aa = bb + rotl(aa + ff(bb, cc, dd) + kk + tt, s);
  };

#define S(aa, bb, cc, dd, k, s, t) step(f, aa, bb, cc, dd, x[k], s, t)
#define S2(aa, bb, cc, dd, k, s, t) step(g, aa, bb, cc, dd, x[k], s, t)
#define S3(aa, bb, cc, dd, k, s, t) step(h, aa, bb, cc, dd, x[k], s, t)
#define S4(aa, bb, cc, dd, k, s, t) step(i, aa, bb, cc, dd, x[k], s, t)

  S(a, b, c, d, 0, 7, 0xd76aa478); S(d, a, b, c, 1, 12, 0xe8c7b756); S(c, d, a, b, 2, 17, 0x242070db);
  S(b, c, d, a, 3, 22, 0xc1bdceee); S(a, b, c, d, 4, 7, 0xf57c0faf); S(d, a, b, c, 5, 12, 0x4787c62a);
  S(c, d, a, b, 6, 17, 0xa8304613); S(b, c, d, a, 7, 22, 0xfd469501); S(a, b, c, d, 8, 7, 0x698098d8);
  S(d, a, b, c, 9, 12, 0x8b44f7af); S(c, d, a, b, 10, 17, 0xffff5bb1); S(b, c, d, a, 11, 22, 0x895cd7be);
  S(a, b, c, d, 12, 7, 0x6b901122); S(d, a, b, c, 13, 12, 0xfd987193); S(c, d, a, b, 14, 17, 0xa679438e);
  S(b, c, d, a, 15, 22, 0x49b40821);

  S2(a, b, c, d, 1, 5, 0xf61e2562); S2(d, a, b, c, 6, 9, 0xc040b340); S2(c, d, a, b, 11, 14, 0x265e5a51);
  S2(b, c, d, a, 0, 20, 0xe9b6c7aa); S2(a, b, c, d, 5, 5, 0xd62f105d); S2(d, a, b, c, 10, 9, 0x02441453);
  S2(c, d, a, b, 15, 14, 0xd8a1e681); S2(b, c, d, a, 4, 20, 0xe7d3fbc8); S2(a, b, c, d, 9, 5, 0x21e1cde6);
  S2(d, a, b, c, 14, 9, 0xc33707d6); S2(c, d, a, b, 3, 14, 0xf4d50d87); S2(b, c, d, a, 8, 20, 0x455a14ed);
  S2(a, b, c, d, 13, 5, 0xa9e3e905); S2(d, a, b, c, 2, 9, 0xfcefa3f8); S2(c, d, a, b, 7, 14, 0x676f02d9);
  S2(b, c, d, a, 12, 20, 0x8d2a4c8a);

  S3(a, b, c, d, 5, 4, 0xfffa3942); S3(d, a, b, c, 8, 11, 0x8771f681); S3(c, d, a, b, 11, 16, 0x6d9d6122);
  S3(b, c, d, a, 14, 23, 0xfde5380c); S3(a, b, c, d, 1, 4, 0xa4beea44); S3(d, a, b, c, 4, 11, 0x4bdecfa9);
  S3(c, d, a, b, 7, 16, 0xf6bb4b60); S3(b, c, d, a, 10, 23, 0xbebfbc70); S3(a, b, c, d, 13, 4, 0x289b7ec6);
  S3(d, a, b, c, 0, 11, 0xeaa127fa); S3(c, d, a, b, 3, 16, 0xd4ef3085); S3(b, c, d, a, 6, 23, 0x04881d05);
  S3(a, b, c, d, 9, 4, 0xd9d4d039); S3(d, a, b, c, 12, 11, 0xe6db99e5); S3(c, d, a, b, 15, 16, 0x1fa27cf8);
  S3(b, c, d, a, 2, 23, 0xc4ac5665);

  S4(a, b, c, d, 0, 6, 0xf4292244); S4(d, a, b, c, 7, 10, 0x432aff97); S4(c, d, a, b, 14, 15, 0xab9423a7);
  S4(b, c, d, a, 5, 21, 0xfc93a039); S4(a, b, c, d, 12, 6, 0x655b59c3); S4(d, a, b, c, 3, 10, 0x8f0ccc92);
  S4(c, d, a, b, 10, 15, 0xffeff47d); S4(b, c, d, a, 1, 21, 0x85845dd1); S4(a, b, c, d, 8, 6, 0x6fa87e4f);
  S4(d, a, b, c, 15, 10, 0xfe2ce6e0); S4(c, d, a, b, 6, 15, 0xa3014314); S4(b, c, d, a, 13, 21, 0x4e0811a1);
  S4(a, b, c, d, 4, 6, 0xf7537e82); S4(d, a, b, c, 11, 10, 0xbd3af235); S4(c, d, a, b, 2, 15, 0x2ad7d2bb);
  S4(b, c, d, a, 9, 21, 0xeb86d391);

#undef S
#undef S2
#undef S3
#undef S4

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
}

void md5_init(Md5Ctx* ctx) {
  ctx->count[0] = ctx->count[1] = 0;
  ctx->state[0] = 0x67452301;
  ctx->state[1] = 0xefcdab89;
  ctx->state[2] = 0x98badcfe;
  ctx->state[3] = 0x10325476;
}

void md5_update(Md5Ctx* ctx, const uint8_t* data, size_t len) {
  size_t i = (ctx->count[0] >> 3) & 0x3F;
  if ((ctx->count[0] += static_cast<uint32_t>(len << 3)) < (len << 3)) {
    ctx->count[1]++;
  }
  ctx->count[1] += static_cast<uint32_t>(len >> 29);
  size_t part = 64 - i;
  size_t idx = 0;
  if (len >= part) {
    std::memcpy(ctx->buffer + i, data, part);
    md5_transform(ctx->state, ctx->buffer);
    for (idx = part; idx + 63 < len; idx += 64) {
      md5_transform(ctx->state, data + idx);
    }
    i = 0;
  }
  std::memcpy(ctx->buffer + i, data + idx, len - idx);
}

void md5_final(Md5Ctx* ctx, uint8_t digest[16]) {
  uint8_t bits[8];
  for (int i = 0; i < 8; ++i) {
    bits[i] = static_cast<uint8_t>((ctx->count[i >> 2] >> ((i & 3) * 8)) & 0xFF);
  }
  uint8_t pad = 0x80;
  md5_update(ctx, &pad, 1);
  pad = 0;
  while ((ctx->count[0] & 0x1F8) != 0x1C0) {
    md5_update(ctx, &pad, 1);
  }
  md5_update(ctx, bits, 8);
  for (int i = 0; i < 16; ++i) {
    digest[i] = static_cast<uint8_t>((ctx->state[i >> 2] >> ((i & 3) * 8)) & 0xFF);
  }
}

static const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::vector<uint32_t> to_uint32_array(const uint8_t* data, size_t len, bool include_length) {
  size_t n = (len + 3) / 4;
  if (include_length) {
    ++n;
  }
  std::vector<uint32_t> v(n, 0);
  for (size_t i = 0; i < len; ++i) {
    v[i >> 2] |= static_cast<uint32_t>(data[i]) << ((i & 3) * 8);
  }
  if (include_length) {
    v[n - 1] = static_cast<uint32_t>(len);
  }
  return v;
}

std::vector<uint8_t> from_uint32_array(const std::vector<uint32_t>& v, uint32_t orig_len) {
  std::vector<uint8_t> out(orig_len);
  for (size_t i = 0; i < orig_len; ++i) {
    out[i] = static_cast<uint8_t>((v[i >> 2] >> ((i & 3) * 8)) & 0xFF);
  }
  return out;
}

void btea(uint32_t* v, int n, const uint32_t* key) {
  if (n <= 1) {
    return;
  }
  uint32_t rounds = 6 + 52 / static_cast<uint32_t>(n);
  uint32_t sum = 0;
  uint32_t z = v[n - 1];
  do {
    sum += kDelta;
    uint32_t e = (sum >> 2) & 3;
    for (int p = 0; p < n - 1; ++p) {
      uint32_t y = v[p + 1];
      uint32_t mx = ((z >> 5) ^ (y << 2)) + ((y >> 3) ^ (z << 4)) ^ (sum ^ y) + (key[(p & 3) ^ e] ^ z);
      z = v[p] += mx;
    }
    uint32_t y = v[0];
    uint32_t mx = ((z >> 5) ^ (y << 2)) + ((y >> 3) ^ (z << 4)) ^ (sum ^ y) + (key[((n - 1) & 3) ^ e] ^ z);
    z = v[n - 1] += mx;
  } while (--rounds);
}

void btea_decrypt(uint32_t* v, int n, const uint32_t* key) {
  if (n <= 1) {
    return;
  }
  uint32_t rounds = 6 + 52 / static_cast<uint32_t>(n);
  uint32_t sum = rounds * kDelta;
  uint32_t y = v[0];
  do {
    uint32_t e = (sum >> 2) & 3;
    for (int p = n - 1; p > 0; --p) {
      uint32_t z = v[p - 1];
      uint32_t mx = ((z >> 5) ^ (y << 2)) + ((y >> 3) ^ (z << 4)) ^ (sum ^ y) + (key[(p & 3) ^ e] ^ z);
      y = v[p] -= mx;
    }
    uint32_t z = v[n - 1];
    uint32_t mx = ((z >> 5) ^ (y << 2)) + ((y >> 3) ^ (z << 4)) ^ (sum ^ y) + (key[(0 & 3) ^ e] ^ z);
    y = v[0] -= mx;
    sum -= kDelta;
  } while (--rounds);
}

}  // namespace

std::string md5_hex(const std::string& input) {
  Md5Ctx ctx;
  md5_init(&ctx);
  md5_update(&ctx, reinterpret_cast<const uint8_t*>(input.data()), input.size());
  uint8_t digest[16];
  md5_final(&ctx, digest);
  static const char hex[] = "0123456789abcdef";
  std::string out(32, '\0');
  for (int i = 0; i < 16; ++i) {
    out[i * 2] = hex[digest[i] >> 4];
    out[i * 2 + 1] = hex[digest[i] & 0xF];
  }
  return out;
}

std::string calc_mtt(uint32_t data_account) {
  return md5_hex(std::to_string(data_account) + kMttSalt);
}

std::string get_zhiling_psw(int app_version) {
  return md5_hex(std::to_string(app_version)).substr(0, 16);
}

std::string base64_encode(const std::vector<uint8_t>& data) {
  std::string out;
  out.reserve(((data.size() + 2) / 3) * 4);
  size_t i = 0;
  while (i + 2 < data.size()) {
    uint32_t n = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8) |
                 static_cast<uint32_t>(data[i + 2]);
    out.push_back(kB64[(n >> 18) & 63]);
    out.push_back(kB64[(n >> 12) & 63]);
    out.push_back(kB64[(n >> 6) & 63]);
    out.push_back(kB64[n & 63]);
    i += 3;
  }
  if (i < data.size()) {
    uint32_t n = static_cast<uint32_t>(data[i]) << 16;
    if (i + 1 < data.size()) {
      n |= static_cast<uint32_t>(data[i + 1]) << 8;
    }
    out.push_back(kB64[(n >> 18) & 63]);
    out.push_back(kB64[(n >> 12) & 63]);
    if (i + 1 < data.size()) {
      out.push_back(kB64[(n >> 6) & 63]);
      out.push_back('=');
    } else {
      out.push_back(kB64[(n >> 6) & 63]);
      out.push_back('=');
    }
  }
  return out;
}

std::vector<uint8_t> base64_decode(const std::string& encoded) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::vector<uint8_t> out;
  int buf = 0, bits = 0;
  for (char c : encoded) {
    if (c == '=' || c == '\n' || c == '\r' || c == ' ') {
      continue;
    }
    int v = val(c);
    if (v < 0) {
      continue;
    }
    buf = (buf << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
    }
  }
  return out;
}

std::vector<uint8_t> xxtea_encrypt(const std::vector<uint8_t>& data, const std::string& key) {
  if (data.empty()) {
    return {};
  }
  auto v = to_uint32_array(data.data(), data.size(), true);
  auto k = to_uint32_array(reinterpret_cast<const uint8_t*>(key.data()), key.size(), false);
  if (k.size() < 4) {
    k.resize(4, 0);
  }
  btea(v.data(), static_cast<int>(v.size()), k.data());
  std::vector<uint8_t> out(v.size() * 4);
  for (size_t i = 0; i < v.size(); ++i) {
    out[i * 4] = static_cast<uint8_t>(v[i] & 0xFF);
    out[i * 4 + 1] = static_cast<uint8_t>((v[i] >> 8) & 0xFF);
    out[i * 4 + 2] = static_cast<uint8_t>((v[i] >> 16) & 0xFF);
    out[i * 4 + 3] = static_cast<uint8_t>((v[i] >> 24) & 0xFF);
  }
  return out;
}

std::vector<uint8_t> xxtea_decrypt(const std::vector<uint8_t>& data, const std::string& key) {
  if (data.empty() || (data.size() % 4) != 0) {
    throw std::runtime_error("invalid cipher length");
  }
  std::vector<uint32_t> v(data.size() / 4);
  for (size_t i = 0; i < v.size(); ++i) {
    v[i] = static_cast<uint32_t>(data[i * 4]) | (static_cast<uint32_t>(data[i * 4 + 1]) << 8) |
           (static_cast<uint32_t>(data[i * 4 + 2]) << 16) | (static_cast<uint32_t>(data[i * 4 + 3]) << 24);
  }
  auto k = to_uint32_array(reinterpret_cast<const uint8_t*>(key.data()), key.size(), false);
  if (k.size() < 4) {
    k.resize(4, 0);
  }
  btea_decrypt(v.data(), static_cast<int>(v.size()), k.data());
  if (v.empty()) {
    throw std::runtime_error("empty block");
  }
  uint32_t orig_len = v.back();
  if (orig_len > (v.size() - 1) * 4) {
    throw std::runtime_error("invalid plain length");
  }
  return from_uint32_array(v, orig_len);
}

std::string encrypt_payload(const std::string& plain_json, int app_version) {
  const std::string key = get_zhiling_psw(app_version);
  std::vector<uint8_t> plain(plain_json.begin(), plain_json.end());
  auto encrypted = xxtea_encrypt(plain, key);
  return base64_encode(encrypted);
}

std::string decrypt_payload(const std::string& cipher_b64, int app_version) {
  const std::string key = get_zhiling_psw(app_version);
  auto raw = base64_decode(cipher_b64);
  auto plain = xxtea_decrypt(raw, key);
  return std::string(plain.begin(), plain.end());
}

bool is_encrypted_body_action(const std::string& action) {
  static const char* kActions[] = {
      "getInitData", "login", "register", "regist", "mail", "uploadSave", "downloadSave", "findSave", "reportChong",
      "recvJiHuoMa", "selTopFightPower", "uploadFightPower", "lunJianFightEnd", "lunJianFindEnemy", "selTopLunJian",
      "selTopWuDao", "findEnemy", "wuDaoFightEnd", "idCard",
  };
  for (const char* a : kActions) {
    if (action == a) {
      return true;
    }
  }
  return false;
}

bool is_encrypted_response_action(const std::string& action) {
  return action == "getInitData";
}

}  // namespace jh::crypto
