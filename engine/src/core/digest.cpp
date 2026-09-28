#include "core/digest.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace pb {

namespace {

constexpr uint32_t kRound[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

}  // namespace

Sha256::Sha256() : h_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

void Sha256::block(const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = static_cast<uint32_t>(p[4 * i]) << 24 | static_cast<uint32_t>(p[4 * i + 1]) << 16 |
               static_cast<uint32_t>(p[4 * i + 2]) << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kRound[i] + w[i];
        const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
}

void Sha256::update(const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    total_ += size;
    while (size) {
        const size_t take = std::min(size, sizeof buffer_ - buffered_);
        std::memcpy(buffer_ + buffered_, p, take);
        buffered_ += take;
        p += take;
        size -= take;
        if (buffered_ == sizeof buffer_) {
            block(buffer_);
            buffered_ = 0;
        }
    }
}

std::string Sha256::hex() {
    const uint64_t bits = total_ * 8;
    const uint8_t one = 0x80, zero = 0;
    update(&one, 1);
    while (buffered_ != 56) update(&zero, 1);
    uint8_t length[8];
    for (int i = 0; i < 8; ++i) length[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    update(length, 8);
    std::string out;
    char word[9];
    for (uint32_t v : h_) {
        std::snprintf(word, sizeof word, "%08x", v);
        out += word;
    }
    return out;
}

std::string sha256_hex(const void* data, size_t size) {
    Sha256 s;
    s.update(data, size);
    return s.hex();
}

std::string base64_encode(const uint8_t* p, size_t n) {
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = static_cast<uint32_t>(p[i]) << 16 | (i + 1 < n ? static_cast<uint32_t>(p[i + 1]) << 8 : 0) |
                           (i + 2 < n ? p[i + 2] : 0);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += i + 1 < n ? kAlphabet[(v >> 6) & 63] : '=';
        out += i + 2 < n ? kAlphabet[v & 63] : '=';
    }
    return out;
}

bool base64_decode(const std::string& text, std::vector<uint8_t>& out) {
    out.clear();
    uint32_t acc = 0;
    int bits = 0, padding = 0;
    for (const unsigned char c : text) {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        if (c == '=') {
            ++padding;
            continue;
        }
        if (padding) return false;  // data after padding
        int v;
        if (c >= 'A' && c <= 'Z')
            v = c - 'A';
        else if (c >= 'a' && c <= 'z')
            v = c - 'a' + 26;
        else if (c >= '0' && c <= '9')
            v = c - '0' + 52;
        else if (c == '+' || c == '-')
            v = 62;
        else if (c == '/' || c == '_')
            v = 63;
        else
            return false;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>(acc >> bits));
        }
    }
    return padding <= 2 && bits < 6;
}

}  // namespace pb
