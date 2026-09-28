// A compact DEFLATE decoder: canonical Huffman codes decoded bit by bit (the method of RFC 1951 section 3.2.2), every
// read bounds-checked, output capped. Speed is secondary: archives are rare and the model dominates the time.
#include "detect/inflate.h"

namespace pb::detect {

namespace {

struct Corrupt {};
struct TooLarge {};

class BitReader {
public:
    BitReader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    uint32_t bits(int count) {
        uint32_t v = buffer_;
        while (have_ < count) {
            if (pos_ >= n_) throw Corrupt();
            v |= static_cast<uint32_t>(p_[pos_++]) << have_;
            have_ += 8;
        }
        buffer_ = count == 32 ? 0 : v >> count;
        have_ -= count;
        return count == 32 ? v : v & ((1u << count) - 1);
    }
    void align() {  // drop the bits left in the current byte
        buffer_ = 0;
        have_ = 0;
    }
    size_t position() const { return pos_; }
    const uint8_t* data() const { return p_; }
    size_t size() const { return n_; }
    void skip(size_t n) { pos_ += n; }

private:
    const uint8_t* p_;
    size_t n_, pos_ = 0;
    uint32_t buffer_ = 0;
    int have_ = 0;
};

// A canonical Huffman code: how many codes of each length, and the symbols sorted by code.
struct Huffman {
    uint16_t count[16] = {0};
    uint16_t symbol[320] = {0};

    void build(const uint8_t* lengths, int n) {
        for (int i = 0; i < n; ++i) ++count[lengths[i]];
        count[0] = 0;
        int left = 1;  // an over-subscribed code is invalid; an incomplete one is allowed (single-code trees)
        for (int len = 1; len < 16; ++len) {
            left <<= 1;
            left -= count[len];
            if (left < 0) throw Corrupt();
        }
        uint16_t offset[16];
        offset[1] = 0;
        for (int len = 1; len < 15; ++len) offset[len + 1] = static_cast<uint16_t>(offset[len] + count[len]);
        for (int i = 0; i < n; ++i)
            if (lengths[i]) symbol[offset[lengths[i]]++] = static_cast<uint16_t>(i);
    }

    int decode(BitReader& in) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= static_cast<int>(in.bits(1));
            const int n = count[len];
            if (code - n < first) return symbol[index + (code - first)];
            index += n;
            first += n;
            first <<= 1;
            code <<= 1;
        }
        throw Corrupt();
    }
};

const uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                  31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                  2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistanceBase[30] = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                    33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void put(std::vector<uint8_t>& out, uint8_t byte, size_t limit) {
    if (out.size() >= limit) throw TooLarge();
    out.push_back(byte);
}

void codes(BitReader& in, const Huffman& literals, const Huffman& distances, std::vector<uint8_t>& out, size_t limit) {
    for (;;) {
        int symbol = literals.decode(in);
        if (symbol < 256) {
            put(out, static_cast<uint8_t>(symbol), limit);
        } else if (symbol == 256) {
            return;
        } else {
            symbol -= 257;
            if (symbol >= 29) throw Corrupt();
            const size_t length = kLengthBase[symbol] + in.bits(kLengthExtra[symbol]);
            const int d = distances.decode(in);
            if (d >= 30) throw Corrupt();
            const size_t distance = kDistanceBase[d] + in.bits(kDistanceExtra[d]);
            if (distance > out.size()) throw Corrupt();
            for (size_t i = 0; i < length; ++i) put(out, out[out.size() - distance], limit);
        }
    }
}

void stored(BitReader& in, std::vector<uint8_t>& out, size_t limit) {
    in.align();
    const size_t at = in.position();
    if (at + 4 > in.size()) throw Corrupt();
    const uint8_t* p = in.data() + at;
    const uint32_t length = p[0] | (p[1] << 8), inverse = p[2] | (p[3] << 8);
    if ((length ^ 0xFFFF) != inverse || at + 4 + length > in.size()) throw Corrupt();
    if (out.size() + length > limit) throw TooLarge();
    out.insert(out.end(), p + 4, p + 4 + length);
    in.skip(4 + length);
}

void fixed(BitReader& in, std::vector<uint8_t>& out, size_t limit) {
    static const struct Tables {
        Huffman literals, distances;
        Tables() {
            uint8_t lengths[288];
            for (int i = 0; i < 144; ++i) lengths[i] = 8;
            for (int i = 144; i < 256; ++i) lengths[i] = 9;
            for (int i = 256; i < 280; ++i) lengths[i] = 7;
            for (int i = 280; i < 288; ++i) lengths[i] = 8;
            literals.build(lengths, 288);
            for (int i = 0; i < 30; ++i) lengths[i] = 5;
            distances.build(lengths, 30);
        }
    } tables;
    codes(in, tables.literals, tables.distances, out, limit);
}

void dynamic(BitReader& in, std::vector<uint8_t>& out, size_t limit) {
    static const uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    const int n_literals = static_cast<int>(in.bits(5)) + 257, n_distances = static_cast<int>(in.bits(5)) + 1;
    const int n_code_lengths = static_cast<int>(in.bits(4)) + 4;
    if (n_literals > 286 || n_distances > 30) throw Corrupt();
    uint8_t lengths[320] = {0};
    for (int i = 0; i < n_code_lengths; ++i) lengths[kOrder[i]] = static_cast<uint8_t>(in.bits(3));
    Huffman code_lengths;
    code_lengths.build(lengths, 19);
    int i = 0;
    while (i < n_literals + n_distances) {
        const int symbol = code_lengths.decode(in);
        if (symbol < 16) {
            lengths[i++] = static_cast<uint8_t>(symbol);
            continue;
        }
        uint8_t value = 0;
        int repeat;
        if (symbol == 16) {
            if (i == 0) throw Corrupt();
            value = lengths[i - 1];
            repeat = 3 + static_cast<int>(in.bits(2));
        } else if (symbol == 17) {
            repeat = 3 + static_cast<int>(in.bits(3));
        } else {
            repeat = 11 + static_cast<int>(in.bits(7));
        }
        if (i + repeat > n_literals + n_distances) throw Corrupt();
        while (repeat--) lengths[i++] = value;
    }
    if (lengths[256] == 0) throw Corrupt();  // no end-of-block code
    Huffman literals, distances;
    literals.build(lengths, n_literals);
    distances.build(lengths + n_literals, n_distances);
    codes(in, literals, distances, out, limit);
}

}  // namespace

InflateResult inflate(const uint8_t* data, size_t size, size_t limit, std::vector<uint8_t>& out, size_t* consumed) {
    BitReader in(data, size);
    try {
        for (bool last = false; !last;) {
            last = in.bits(1) != 0;
            switch (in.bits(2)) {
                case 0: stored(in, out, limit); break;
                case 1: fixed(in, out, limit); break;
                case 2: dynamic(in, out, limit); break;
                default: return InflateResult::Corrupt;
            }
        }
    } catch (const Corrupt&) {
        return InflateResult::Corrupt;
    } catch (const TooLarge&) {
        return InflateResult::TooLarge;
    }
    if (consumed) *consumed = in.position();
    return InflateResult::Ok;
}

}  // namespace pb::detect
