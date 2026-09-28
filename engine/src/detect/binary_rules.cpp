#include "detect/binary_rules.h"

#include <string>

namespace pb::detect {

namespace {

bool is_base64_byte(uint8_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' ||
           c == '=';
}

bool is_base64(const std::vector<uint8_t>& b, int64_t a, int64_t z) {
    if (a >= z) return false;
    for (int64_t k = a; k < z; ++k)
        if (!is_base64_byte(b[k])) return false;
    return true;
}

// "<algorithm>-Digest", "<algorithm>-Digest-Manifest" or "<algorithm>-Digest-Manifest-Main-Attributes", the
// algorithm made of letters, digits and '-' ("SHA-256", "SHA1", "MD5").
bool is_digest_attribute_name(const std::string& name) {
    for (const std::string suffix : {"-Digest", "-Digest-Manifest", "-Digest-Manifest-Main-Attributes"}) {
        if (name.size() <= suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;
        for (size_t k = 0; k < name.size() - suffix.size(); ++k) {
            const char c = name[k];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
        }
        return true;
    }
    return false;
}

// Characters of the path of an archive entry ("org/example/Keys$Inner.class").
bool is_path(const std::vector<uint8_t>& b, int64_t a, int64_t z) {
    if (a >= z) return false;
    for (int64_t k = a; k < z; ++k) {
        const uint8_t c = b[k];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' || c == '.' ||
              c == '_' || c == '-' || c == '$'))
            return false;
    }
    return true;
}

// The value of a manifest "Name:" attribute: an entry path with at least one '/' or '.'.
bool is_entry_path(const std::vector<uint8_t>& b, int64_t a, int64_t z) {
    if (!is_path(b, a, z)) return false;
    for (int64_t k = a; k < z; ++k)
        if (b[k] == '/' || b[k] == '.') return true;
    return false;
}

// `line` (printable, not a continuation line) is a digest attribute or an entry name, and the continuation lines
// below it hold what its value allows (`base64_tail`: all base64, for a digest; `path_tail`: all path, for a name).
bool attribute_verdict(const std::vector<uint8_t>& b, Range line, bool base64_tail, bool path_tail) {
    const std::string text(b.begin() + line.first, b.begin() + line.second);
    const size_t colon = text.find(": ");
    if (colon == std::string::npos) return false;
    const int64_t value = line.first + static_cast<int64_t>(colon) + 2;
    if (text.compare(0, colon, "Name") == 0) return path_tail && is_entry_path(b, value, line.second);
    return base64_tail && is_digest_attribute_name(text.substr(0, colon)) && is_base64(b, value, line.second);
}

}  // namespace

Range printable_string_around(const std::vector<uint8_t>& b, int64_t a, int64_t z) {
    int64_t i = a, j = z;
    while (i > 0 && b[i - 1] >= 32 && b[i - 1] < 127) --i;
    while (j < static_cast<int64_t>(b.size()) && b[j] >= 32 && b[j] < 127) ++j;
    return {i, j};
}

bool touches_printable_string(const std::vector<uint8_t>& b, int64_t a, int64_t z, int64_t L) {
    const int64_t n = static_cast<int64_t>(b.size());
    auto printable = [&](int64_t k) { return (b[k] >= 32 && b[k] < 127) || b[k] == 9; };
    // Each run meeting [a, z) is measured only as far as L: the answer is the same, and a span inside a long string
    // does not read the whole string.
    for (int64_t i = a; i < z;) {  // ASCII runs meeting [a, z)
        if (!printable(i)) {
            ++i;
            continue;
        }
        int64_t e = i;
        while (e < n && printable(e) && e - i < L) ++e;
        if (e - i >= L) return true;
        int64_t s = i;  // e is the end of the run now
        while (s > 0 && printable(s - 1) && e - s < L) --s;
        if (e - s >= L) return true;
        i = e;
    }
    auto utf16_char = [&](int64_t k) { return 0 <= k && k + 1 < n && printable(k) && b[k + 1] == 0; };
    for (int64_t parity = 0; parity < 2; ++parity) {  // UTF-16LE runs at offsets of each parity
        const int64_t phase = (((a - 1 - parity) % 2) + 2) % 2;
        for (int64_t k = phase == 0 ? a - 1 : a; k < z;) {
            if (!utf16_char(k)) {
                k += 2;
                continue;
            }
            int64_t e = k;
            while (utf16_char(e) && (e - k) / 2 < L) e += 2;
            if ((e - k) / 2 >= L) return true;
            int64_t s = k;
            while (utf16_char(s - 2) && (e - s) / 2 < L) s -= 2;
            if ((e - s) / 2 >= L) return true;
            k = e;
        }
    }
    return false;
}

Range PrintableStrings::around(int64_t a, int64_t z) {
    const int64_t n = static_cast<int64_t>(b_.size());
    auto printable = [&](int64_t k) { return b_[k] >= 32 && b_[k] < 127; };
    int64_t i = a;
    if (last_a_ >= 0 && a >= last_a_) {
        while (i > last_a_ && printable(i - 1)) --i;
        if (i == last_a_) i = last_left_;  // [last_a_, a) is printable: the previous walk went on from there
    } else {
        while (i > 0 && printable(i - 1)) --i;
    }
    int64_t j = z;
    if (last_z_ >= 0 && z >= last_z_ && z <= last_right_) {
        j = last_right_;  // [last_z_, last_right_) is printable and ends the run
    } else {
        while (j < n && printable(j)) ++j;
    }
    last_a_ = a;
    last_left_ = i;
    last_z_ = z;
    last_right_ = j;
    return {i, j};
}

bool ManifestLines::metadata(Range line) {
    const auto known = verdicts_.find(line);
    if (known != verdicts_.end()) return known->second;
    const Range given = line;
    const std::vector<uint8_t>& b = b_;
    auto printable = [&](int64_t k) { return b[k] >= 32 && b[k] < 127; };
    auto verdict = [&](bool v) {
        verdicts_[given] = v;
        return v;
    };
    // As is_jar_manifest_metadata: the line break a span took in is trimmed; a line with other bytes is not metadata.
    while (line.first < line.second && !printable(line.first)) ++line.first;
    while (line.first < line.second && !printable(line.second - 1)) --line.second;
    for (int64_t k = line.first; k < line.second; ++k)
        if (!printable(k)) return verdict(false);
    // A printable string is known by its start. The answer of a line depends on it and on what the continuation lines
    // below it hold: each (line, tails) is decided once, for every span of the document.
    bool base64_tail = true, path_tail = true, result = false;
    std::vector<std::tuple<int64_t, bool, bool>> walked;
    for (;;) {
        const auto key = std::make_tuple(line.first, base64_tail, path_tail);
        const auto hit = walks_.find(key);
        if (hit != walks_.end()) {
            result = hit->second;
            break;
        }
        walked.push_back(key);
        if (!(line.first < line.second && b[line.first] == ' ')) {  // the attribute line
            result = attribute_verdict(b, line, base64_tail, path_tail);
            break;
        }
        base64_tail = base64_tail && is_base64(b, line.first + 1, line.second);
        path_tail = path_tail && is_path(b, line.first + 1, line.second);
        if (!base64_tail && !path_tail) break;
        int64_t k = line.first;
        if (k == 0 || b[k - 1] != '\n') break;
        --k;
        if (k > 0 && b[k - 1] == '\r') --k;
        if (k == 0 || b[k - 1] < 32 || b[k - 1] >= 127) break;
        line = printable_string_around(b, k - 1, k);
    }
    for (const auto& key : walked) walks_[key] = result;
    return verdict(result);
}

bool same_ascii_text(const std::vector<uint8_t>& b, Range x, Range y) {
    if (x == y) return true;
    if (x.second - x.first != y.second - y.first) return false;  // one symbol per byte: lengths must match
    for (int64_t i = 0; i < x.second - x.first; ++i) {
        const uint8_t p = b[x.first + i], q = b[y.first + i];
        if (p != q && (p < 0x80 || q < 0x80)) return false;
    }
    return true;
}

bool is_jar_manifest_metadata(const std::vector<uint8_t>& b, Range line) {
    auto printable = [&](int64_t k) { return b[k] >= 32 && b[k] < 127; };
    // A span may take in the line break around its line ("...=\r"): trim it. A span over several lines is kept.
    while (line.first < line.second && !printable(line.first)) ++line.first;
    while (line.first < line.second && !printable(line.second - 1)) --line.second;
    for (int64_t k = line.first; k < line.second; ++k)
        if (!printable(k)) return false;
    bool base64_tail = true, path_tail = true;                  // what the continuation lines hold, if any
    while (line.first < line.second && b[line.first] == ' ') {  // a continuation line: back to the line before
        base64_tail = base64_tail && is_base64(b, line.first + 1, line.second);
        path_tail = path_tail && is_path(b, line.first + 1, line.second);
        if (!base64_tail && !path_tail) return false;
        int64_t k = line.first;
        if (k == 0 || b[k - 1] != '\n') return false;
        --k;
        if (k > 0 && b[k - 1] == '\r') --k;
        if (k == 0 || b[k - 1] < 32 || b[k - 1] >= 127) return false;
        line = printable_string_around(b, k - 1, k);
    }
    return attribute_verdict(b, line, base64_tail, path_tail);
}

}  // namespace pb::detect
