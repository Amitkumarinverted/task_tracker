// TaskFlow Server - minimal crypto helpers (SHA-256, HMAC, PBKDF2, secure random)
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace tf {

std::vector<uint8_t> sha256(const uint8_t* data, size_t len);
std::string sha256_hex(const std::string& s);
std::vector<uint8_t> hmac_sha256(const std::vector<uint8_t>& key, const uint8_t* msg, size_t len);
std::vector<uint8_t> pbkdf2_sha256(const std::string& password, const std::vector<uint8_t>& salt,
                                   uint32_t iterations, size_t out_len);
std::vector<uint8_t> secure_random(size_t n);
std::string to_hex(const std::vector<uint8_t>& v);
std::vector<uint8_t> from_hex(const std::string& h);
bool constant_time_equal(const std::string& a, const std::string& b);

}  // namespace tf
