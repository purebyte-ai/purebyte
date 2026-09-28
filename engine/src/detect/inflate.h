// DEFLATE decompression (RFC 1951), for scanning inside zip and gzip archives without a third-party library.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pb::detect {

enum class InflateResult { Ok, Corrupt, TooLarge };

// Decompresses the raw DEFLATE stream at in[0, size) into `out`, stopping with TooLarge as soon as the output would
// exceed `limit` bytes (the defence against decompression bombs). `consumed` receives the bytes of input used.
InflateResult inflate(const uint8_t* in, size_t size, size_t limit, std::vector<uint8_t>& out,
                      size_t* consumed = nullptr);

}  // namespace pb::detect
