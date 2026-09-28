// SHA-256 (FIPS 180-4) and base64 (RFC 4648): small, dependency-free, used for checksums of model files and reports.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pb {

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t size);
    std::string hex();  // finishes the hash; the object must not be updated afterwards

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buffer_[64];
    size_t buffered_ = 0;
    uint64_t total_ = 0;
};

std::string sha256_hex(const void* data, size_t size);

std::string base64_encode(const uint8_t* data, size_t size);
// Standard or URL-safe alphabet; whitespace ignored; padding optional but never before data. False when invalid.
bool base64_decode(const std::string& text, std::vector<uint8_t>& out);

}  // namespace pb
