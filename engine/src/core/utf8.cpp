#include "core/utf8.h"

namespace pb::utf8 {

void append(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::string decode_replace(const uint8_t* p, size_t n, size_t* code_points) {
    static const char kReplacement[] = "\xEF\xBF\xBD";
    std::string out;
    out.reserve(n);
    size_t i = 0, count = 0;
    auto continuation = [&](size_t k) { return k < n && (p[k] & 0xC0) == 0x80; };
    while (i < n) {
        const uint8_t c = p[i];
        ++count;
        if (c < 0x80) {
            out += static_cast<char>(c);
            ++i;
        } else if (c < 0xC2) {
            out += kReplacement;
            ++i;
        } else if (c < 0xE0) {
            if (continuation(i + 1)) {
                out.append(reinterpret_cast<const char*>(p + i), 2);
                i += 2;
            } else {
                out += kReplacement;
                ++i;
            }
        } else if (c < 0xF0) {
            // E0 needs A0..BF next (no overlong forms), ED needs 80..9F (no surrogates)
            const uint8_t lo = c == 0xE0 ? 0xA0 : 0x80, hi = c == 0xED ? 0x9F : 0xBF;
            if (i + 1 < n && p[i + 1] >= lo && p[i + 1] <= hi) {
                if (continuation(i + 2)) {
                    out.append(reinterpret_cast<const char*>(p + i), 3);
                    i += 3;
                } else {
                    out += kReplacement;
                    i += 2;
                }
            } else {
                out += kReplacement;
                ++i;
            }
        } else if (c < 0xF5) {
            // F0 needs 90..BF next (no overlong forms), F4 needs 80..8F (nothing above U+10FFFF)
            const uint8_t lo = c == 0xF0 ? 0x90 : 0x80, hi = c == 0xF4 ? 0x8F : 0xBF;
            if (i + 1 < n && p[i + 1] >= lo && p[i + 1] <= hi) {
                if (continuation(i + 2)) {
                    if (continuation(i + 3)) {
                        out.append(reinterpret_cast<const char*>(p + i), 4);
                        i += 4;
                    } else {
                        out += kReplacement;
                        i += 3;
                    }
                } else {
                    out += kReplacement;
                    i += 2;
                }
            } else {
                out += kReplacement;
                ++i;
            }
        } else {
            out += kReplacement;
            ++i;
        }
    }
    if (code_points) *code_points = count;
    return out;
}

std::string sanitize(const std::string& s) {
    return decode_replace(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

std::string printable(const std::string& s) {
    static const char kHex[] = "0123456789abcdef";
    const std::string valid = sanitize(s);
    std::string out;
    out.reserve(valid.size());
    for (size_t i = 0; i < valid.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(valid[i]);
        if (c < 0x20 || c == 0x7F) {
            out += "\\x";
            out += kHex[c >> 4];
            out += kHex[c & 15];
        } else if (c == 0xC2 && i + 1 < valid.size() && static_cast<unsigned char>(valid[i + 1]) <= 0x9F) {
            // U+0080..U+009F (valid UTF-8 C2 80..C2 9F): the C1 controls, CSI (U+009B) among them.
            const unsigned char low = static_cast<unsigned char>(valid[++i]);
            out += "\\u00";
            out += kHex[low >> 4];
            out += kHex[low & 15];
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

size_t count_code_points(const std::string& valid) {
    size_t n = 0;
    for (const unsigned char c : valid) n += (c & 0xC0) != 0x80;
    return n;
}

}  // namespace pb::utf8
