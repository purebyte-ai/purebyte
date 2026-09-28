#include "detect/text.h"

#include <algorithm>
#include <cstring>

#include "core/utf8.h"

namespace pb::detect {

std::string text_of(const std::vector<uint8_t>& b, int64_t a, int64_t z, size_t* code_points) {
    a = std::max<int64_t>(a, 0);
    z = std::min<int64_t>(z, static_cast<int64_t>(b.size()));
    return utf8::decode_replace(b.data() + a, z > a ? static_cast<size_t>(z - a) : 0, code_points);
}

std::string ascii_text(const uint8_t* p, size_t n) {
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (p[i] < 0x80)
            out += static_cast<char>(p[i]);
        else
            out += "\xEF\xBF\xBD";
    }
    return out;
}

namespace {

// Python 3's str.isspace(): the whitespace characters of Unicode (bidirectional classes WS, B, S and category Zs).
bool is_space(uint32_t c) {
    return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

uint32_t decode_at(const std::string& s, size_t i, size_t* length) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
        *length = 1;
        return c;
    }
    if (c < 0xE0) {
        *length = 2;
        return ((c & 0x1Fu) << 6) | (s[i + 1] & 0x3Fu);
    }
    if (c < 0xF0) {
        *length = 3;
        return ((c & 0x0Fu) << 12) | ((s[i + 1] & 0x3Fu) << 6) | (s[i + 2] & 0x3Fu);
    }
    *length = 4;
    return ((c & 0x07u) << 18) | ((s[i + 1] & 0x3Fu) << 12) | ((s[i + 2] & 0x3Fu) << 6) | (s[i + 3] & 0x3Fu);
}

}  // namespace

std::string strip(const std::string& s) {
    size_t lo = s.size(), hi = 0;  // where the first code point that is not a space starts, and the last one ends
    for (size_t i = 0, n; i < s.size(); i += n)
        if (!is_space(decode_at(s, i, &n))) {
            if (lo == s.size()) lo = i;
            hi = i + n;
        }
    return lo < hi ? s.substr(lo, hi - lo) : std::string();
}

Range context_range(const std::vector<uint8_t>& b, int64_t a, int64_t z, int64_t line_start, int64_t line_end) {
    auto continuation = [&](int64_t i) { return (b[static_cast<size_t>(i)] & 0xC0) == 0x80; };
    int64_t lo = line_start, hi = line_end;
    if (a - lo > kContextMarginBytes) {
        lo = a - kContextMarginBytes;
        for (int k = 0; k < 3 && lo < a && continuation(lo); ++k) ++lo;
    }
    const int64_t reach = std::min(std::max(z, a), a + kContextSpanBytes) + kContextMarginBytes;
    if (hi > reach) {
        hi = reach;
        for (int k = 0; k < 3 && hi > a && continuation(hi); ++k) --hi;
    }
    return {lo, hi};
}

Range context_range(const std::vector<uint8_t>& b, int64_t a, int64_t z) {
    const int64_t n = static_cast<int64_t>(b.size());
    // One byte further than the bounds: a line that reaches it is cut there, wherever it really starts or ends.
    const int64_t floor = std::max<int64_t>(0, a - kContextMarginBytes - 1);
    const int64_t ceiling = std::min(n, std::min(std::max(z, a), a + kContextSpanBytes) + kContextMarginBytes + 1);
    const int64_t before = rfind_byte(b, '\n', floor, a), after = find_byte(b, '\n', a, ceiling);
    return context_range(b, a, z, before >= 0 ? before + 1 : floor, after >= 0 ? after : ceiling);
}

bool utf16_to_utf8(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
    if (in.size() < 2) return false;
    bool little;
    if (in[0] == 0xFF && in[1] == 0xFE)
        little = true;
    else if (in[0] == 0xFE && in[1] == 0xFF)
        little = false;
    else
        return false;
    if ((in.size() - 2) % 2) return false;  // an odd tail: not UTF-16 after all, scanned as it is
    auto unit = [&](size_t i) {
        return little ? static_cast<uint32_t>(in[i] | (in[i + 1] << 8))
                      : static_cast<uint32_t>((in[i] << 8) | in[i + 1]);
    };
    std::string text;
    text.reserve(in.size());
    for (size_t i = 2; i < in.size(); i += 2) {
        const uint32_t u = unit(i);
        uint32_t cp;
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (i + 3 >= in.size()) return false;  // high surrogate at the end
            const uint32_t low = unit(i + 2);
            if (low < 0xDC00 || low > 0xDFFF) return false;
            cp = 0x10000 + ((u - 0xD800) << 10) + (low - 0xDC00);
            i += 2;
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            return false;  // lone low surrogate
        } else {
            cp = u;
        }
        utf8::append(text, cp);
    }
    out.assign(text.begin(), text.end());
    return true;
}

LineIndex::LineIndex(const std::vector<uint8_t>& b) : size_(static_cast<int64_t>(b.size())) {
    for (int64_t i = 0; i < size_; ++i)
        if (b[i] == '\n') newlines_.push_back(i);
}

void LineIndex::locate(int64_t pos, int64_t& line, int64_t& col, int64_t& line_start, int64_t& line_end) const {
    const size_t k = static_cast<size_t>(std::lower_bound(newlines_.begin(), newlines_.end(), pos) - newlines_.begin());
    const int64_t previous = k ? newlines_[k - 1] : -1;
    line = static_cast<int64_t>(k) + 1;
    col = pos - (previous + 1) + 1;
    line_start = previous + 1;
    line_end = k < newlines_.size() ? newlines_[k] : size_;
}

int64_t LineIndex::line_start(int64_t pos) const {
    const auto k = std::lower_bound(newlines_.begin(), newlines_.end(), pos);
    return k == newlines_.begin() ? 0 : *(k - 1) + 1;
}

int64_t LineIndex::next_newline(int64_t pos) const {
    const auto k = std::lower_bound(newlines_.begin(), newlines_.end(), pos);
    return k == newlines_.end() ? size_ : *k;
}

int64_t find_byte(const std::vector<uint8_t>& b, uint8_t c, int64_t lo, int64_t hi) {
    lo = std::max<int64_t>(lo, 0);
    hi = std::min<int64_t>(hi, static_cast<int64_t>(b.size()));
    if (lo >= hi) return -1;
    const void* p = std::memchr(b.data() + lo, c, static_cast<size_t>(hi - lo));
    return p ? static_cast<const uint8_t*>(p) - b.data() : -1;
}

int64_t rfind_byte(const std::vector<uint8_t>& b, uint8_t c, int64_t lo, int64_t hi) {
    lo = std::max<int64_t>(lo, 0);
    hi = std::min<int64_t>(hi, static_cast<int64_t>(b.size()));
    for (int64_t i = hi - 1; i >= lo; --i)
        if (b[i] == c) return i;
    return -1;
}

int64_t find_bytes(const std::vector<uint8_t>& b, const char* s, int64_t lo) {
    const int64_t m = static_cast<int64_t>(std::strlen(s)), n = static_cast<int64_t>(b.size());
    for (int64_t i = std::max<int64_t>(lo, 0); i + m <= n; ++i) {
        const void* p = std::memchr(b.data() + i, static_cast<uint8_t>(s[0]), static_cast<size_t>(n - i));
        if (!p) return -1;
        i = static_cast<const uint8_t*>(p) - b.data();
        if (i + m > n) return -1;
        if (!std::memcmp(b.data() + i, s, static_cast<size_t>(m))) return i;
    }
    return -1;
}

}  // namespace pb::detect
