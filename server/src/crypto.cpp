// TaskFlow Server - minimal crypto helpers
#include "crypto.h"

#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <cstdio>
#endif

namespace tf {
namespace {

const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

struct Sha256 {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint8_t buf[64];
  size_t buf_len = 0;
  uint64_t total = 0;

  void block(const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
      w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
      uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
      uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      uint32_t ch = (e & f) ^ (~e & g);
      uint32_t t1 = hh + S1 + ch + K[i] + w[i];
      uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t t2 = S0 + mj;
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  void update(const uint8_t* p, size_t n) {
    total += n;
    while (n > 0) {
      size_t take = 64 - buf_len < n ? 64 - buf_len : n;
      memcpy(buf + buf_len, p, take);
      buf_len += take; p += take; n -= take;
      if (buf_len == 64) { block(buf); buf_len = 0; }
    }
  }
  std::vector<uint8_t> finish() {
    uint64_t bits = total * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t zero = 0;
    while (buf_len != 56) update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    update(len, 8);
    std::vector<uint8_t> out(32);
    for (int i = 0; i < 8; i++) {
      out[i * 4] = h[i] >> 24; out[i * 4 + 1] = h[i] >> 16; out[i * 4 + 2] = h[i] >> 8; out[i * 4 + 3] = h[i];
    }
    return out;
  }
};

}  // namespace

std::vector<uint8_t> sha256(const uint8_t* data, size_t len) {
  Sha256 s;
  s.update(data, len);
  return s.finish();
}

std::string sha256_hex(const std::string& s) {
  return to_hex(sha256(reinterpret_cast<const uint8_t*>(s.data()), s.size()));
}

std::vector<uint8_t> hmac_sha256(const std::vector<uint8_t>& key_in, const uint8_t* msg, size_t len) {
  std::vector<uint8_t> key = key_in;
  if (key.size() > 64) key = sha256(key.data(), key.size());
  key.resize(64, 0);
  uint8_t ipad[64], opad[64];
  for (int i = 0; i < 64; i++) { ipad[i] = key[i] ^ 0x36; opad[i] = key[i] ^ 0x5c; }
  Sha256 inner;
  inner.update(ipad, 64);
  inner.update(msg, len);
  auto ih = inner.finish();
  Sha256 outer;
  outer.update(opad, 64);
  outer.update(ih.data(), ih.size());
  return outer.finish();
}

std::vector<uint8_t> pbkdf2_sha256(const std::string& password, const std::vector<uint8_t>& salt,
                                   uint32_t iterations, size_t out_len) {
  std::vector<uint8_t> key(password.begin(), password.end());
  if (key.size() > 64) key = sha256(key.data(), key.size());
  // precompute padded keys for speed
  std::vector<uint8_t> k = key;
  k.resize(64, 0);
  uint8_t ipad[64], opad[64];
  for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
  Sha256 ibase, obase;
  ibase.update(ipad, 64);
  obase.update(opad, 64);
  auto prf = [&](const uint8_t* m, size_t n) {
    Sha256 a = ibase;
    a.update(m, n);
    auto ih = a.finish();
    Sha256 b = obase;
    b.update(ih.data(), ih.size());
    return b.finish();
  };
  std::vector<uint8_t> out;
  for (uint32_t blk = 1; out.size() < out_len; blk++) {
    std::vector<uint8_t> s = salt;
    s.push_back(blk >> 24); s.push_back(blk >> 16); s.push_back(blk >> 8); s.push_back(blk);
    auto u = prf(s.data(), s.size());
    auto t = u;
    for (uint32_t i = 1; i < iterations; i++) {
      u = prf(u.data(), u.size());
      for (size_t j = 0; j < t.size(); j++) t[j] ^= u[j];
    }
    out.insert(out.end(), t.begin(), t.end());
  }
  out.resize(out_len);
  return out;
}

std::vector<uint8_t> secure_random(size_t n) {
  std::vector<uint8_t> v(n);
#ifdef _WIN32
  if (BCryptGenRandom(nullptr, v.data(), (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
    throw std::runtime_error("BCryptGenRandom failed");
#else
  FILE* f = fopen("/dev/urandom", "rb");
  if (!f || fread(v.data(), 1, n, f) != n) {
    if (f) fclose(f);
    throw std::runtime_error("cannot read /dev/urandom");
  }
  fclose(f);
#endif
  return v;
}

std::string to_hex(const std::vector<uint8_t>& v) {
  static const char* d = "0123456789abcdef";
  std::string s;
  s.reserve(v.size() * 2);
  for (auto b : v) { s += d[b >> 4]; s += d[b & 15]; }
  return s;
}

std::vector<uint8_t> from_hex(const std::string& h) {
  std::vector<uint8_t> v;
  auto val = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
  };
  for (size_t i = 0; i + 1 < h.size(); i += 2) v.push_back((uint8_t)(val(h[i]) << 4 | val(h[i + 1])));
  return v;
}

bool constant_time_equal(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned char r = 0;
  for (size_t i = 0; i < a.size(); i++) r |= (unsigned char)(a[i] ^ b[i]);
  return r == 0;
}

}  // namespace tf
