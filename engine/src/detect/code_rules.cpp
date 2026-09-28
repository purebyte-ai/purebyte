#include "detect/code_rules.h"

#include <algorithm>
#include <cstring>

namespace pb::detect {

namespace {

inline bool alnum(uint8_t c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
inline bool base64_char(uint8_t c) { return alnum(c) || c == '+' || c == '/'; }
inline bool blank(uint8_t c) { return c == ' ' || c == '\t'; }
inline bool space(uint8_t c) { return c == ' ' || (c >= 9 && c <= 13); }  // \s on bytes: [ \t\n\r\f\v]

int base64_value(uint8_t c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    return c == '+' ? 62 : 63;
}

// End of a private-key header or footer label that starts at i ("<[A-Z0-9 ]*>PRIVATE KEY[ BLOCK]-----"), or -1.
// The label class excludes '-', so the label is the maximal run of [A-Z0-9 ] and must end in one of the two forms.
int64_t key_label_end(const std::vector<uint8_t>& b, int64_t i) {
    const int64_t n = static_cast<int64_t>(b.size());
    int64_t j = i;
    while (j < n && ((b[j] >= 'A' && b[j] <= 'Z') || (b[j] >= '0' && b[j] <= '9') || b[j] == ' ')) ++j;
    auto ends_with = [&](const char* s) {
        const int64_t m = static_cast<int64_t>(std::strlen(s));
        return j - i >= m && !std::memcmp(b.data() + j - m, s, static_cast<size_t>(m));
    };
    if (!ends_with("PRIVATE KEY") && !ends_with("PRIVATE KEY BLOCK")) return -1;
    if (j + 5 > n || std::memcmp(b.data() + j, "-----", 5) != 0) return -1;
    return j + 5;
}

}  // namespace

// [A-Za-z0-9+/]{32,}={0,2}, found left to right: a match is a maximal run of >= 32 alphabet characters plus up to two
// '=' (a shorter run fails at every start inside it). The run is decoded after padding it to a multiple of 4; strict
// decoding rejects more than two '=' in total, and what passes is decoded dropping the leftover bits.
std::vector<Base64Blob> readable_base64_blobs(const std::vector<uint8_t>& b, int limit) {
    std::vector<Base64Blob> out;
    const int64_t n = static_cast<int64_t>(b.size());
    int64_t p = 0;
    while (p < n) {
        if (!base64_char(b[p])) {
            ++p;
            continue;
        }
        int64_t r = p;
        while (r < n && base64_char(b[r])) ++r;
        if (r - p < 32) {
            p = r;
            continue;
        }
        int64_t e = r;
        int equals = 0;
        while (e < n && equals < 2 && b[e] == '=') {
            ++e;
            ++equals;
        }
        if (static_cast<int>(out.size()) >= limit) break;
        const int64_t data = r - p;
        const int64_t pad = (4 - (data + equals) % 4) % 4;
        if (equals + pad <= 2) {
            std::vector<uint8_t> decoded;
            decoded.reserve(static_cast<size_t>(data * 3 / 4 + 2));
            int64_t i = p;
            for (; i + 4 <= r; i += 4) {
                const uint32_t v = (base64_value(b[i]) << 18) | (base64_value(b[i + 1]) << 12) |
                                   (base64_value(b[i + 2]) << 6) | base64_value(b[i + 3]);
                decoded.push_back(static_cast<uint8_t>(v >> 16));
                decoded.push_back(static_cast<uint8_t>(v >> 8));
                decoded.push_back(static_cast<uint8_t>(v));
            }
            const int64_t rest = r - i;
            if (rest == 2) {
                const uint32_t v = (base64_value(b[i]) << 18) | (base64_value(b[i + 1]) << 12);
                decoded.push_back(static_cast<uint8_t>(v >> 16));
            } else if (rest == 3) {
                const uint32_t v =
                    (base64_value(b[i]) << 18) | (base64_value(b[i + 1]) << 12) | (base64_value(b[i + 2]) << 6);
                decoded.push_back(static_cast<uint8_t>(v >> 16));
                decoded.push_back(static_cast<uint8_t>(v >> 8));
            }
            if (decoded.size() >= 12) {
                size_t readable = 0;
                for (uint8_t x : decoded) readable += (x >= 32 && x < 127) || x == 9 || x == 10 || x == 13;
                if (static_cast<double>(readable) / static_cast<double>(decoded.size()) > 0.90)
                    out.push_back({p, e - p, std::move(decoded)});
            }
        }
        p = e;
    }
    return out;
}

// The user part cannot hold ':', so it is the whole prefix before the first ':'; the value is the maximal run of
// non-space, non-colon bytes, and it must end the string or be followed by one final "\n" (what `$` allows).
bool is_basic_auth(const std::vector<uint8_t>& d) {
    const size_t n = d.size();
    auto user_char = [](uint8_t c) { return alnum(c) || c == '.' || c == '_' || c == '@' || c == '+' || c == '-'; };
    size_t i = 0;
    while (i < n && user_char(d[i])) ++i;
    if (i < 1 || i > 64 || i >= n || d[i] != ':') return false;
    size_t j = i + 1;
    while (j < n && !space(d[j]) && d[j] != ':') ++j;
    const size_t value = j - (i + 1);
    if (value < 6 || value > 128) return false;
    return j == n || (j == n - 1 && d[j] == '\n');
}

// The block runs lazily from a matching header to the FIRST following footer whose label matches too.
std::vector<Range> private_key_blocks(const std::vector<uint8_t>& b) {
    std::vector<Range> out;
    int64_t pos = 0;
    for (;;) {
        const int64_t s = find_bytes(b, "-----BEGIN ", pos);
        if (s < 0) break;
        const int64_t header_end = key_label_end(b, s + 11);
        if (header_end < 0) {
            pos = s + 1;
            continue;
        }
        int64_t p = header_end, end = -1;
        for (;;) {
            const int64_t q = find_bytes(b, "-----END ", p);
            if (q < 0) break;
            end = key_label_end(b, q + 9);
            if (end >= 0) break;
            p = q + 1;
        }
        if (end < 0) break;  // no footer after this header: none after any later header either
        out.push_back({s, end});
        pos = end;
    }
    return out;
}

// data:TYPE/SUBTYPE(;PARAM)*;base64,(PAYLOAD{16,}) with TYPE and SUBTYPE excluding '/' and ';' (maximal runs). Each
// (;PARAM) takes a whole segment, so ";base64," can only match as the last segment. The payload is the maximal run
// of [A-Za-z0-9+/=] and whitespace.
std::vector<Range> data_uri_payloads(const std::vector<uint8_t>& b) {
    std::vector<Range> out;
    const int64_t n = static_cast<int64_t>(b.size());
    auto mime_char = [](uint8_t c) { return alnum(c) || c == '.' || c == '+' || c == '-'; };
    auto param_char = [](uint8_t c) { return alnum(c) || c == '=' || c == '.' || c == '_' || c == '-'; };
    auto payload_char = [](uint8_t c) { return alnum(c) || c == '+' || c == '/' || c == '=' || space(c); };
    int64_t pos = 0;
    for (;;) {
        const int64_t s = find_bytes(b, "data:", pos);
        if (s < 0) break;
        int64_t j = s + 5;
        const int64_t type_start = j;
        while (j < n && mime_char(b[j])) ++j;
        bool ok = j > type_start && j < n && b[j] == '/';
        int64_t p = j + 1;
        if (ok) {
            const int64_t subtype_start = p;
            while (p < n && mime_char(b[p])) ++p;
            ok = p > subtype_start;
        }
        if (ok) {
            int64_t last_start = -1, last_end = -1;
            while (p < n && b[p] == ';') {
                int64_t q = p + 1;
                while (q < n && param_char(b[q])) ++q;
                if (q == p + 1) break;
                last_start = p + 1;
                last_end = q;
                p = q;
            }
            ok = last_start >= 0 && last_end - last_start == 6 && !std::memcmp(b.data() + last_start, "base64", 6) &&
                 p < n && b[p] == ',';
        }
        if (ok) {
            const int64_t g = p + 1;
            int64_t e = g;
            while (e < n && payload_char(b[e])) ++e;
            if (e - g >= 16) {
                out.push_back({g, e});
                pos = e;
                continue;
            }
        }
        pos = s + 1;
    }
    return out;
}

// ^([ \t]*[-+]?[ \t]*["']?[A-Za-z_][A-Za-z0-9_.\-]{0,47}["']?[ \t]*(?::=|=>|=|:[ \t])[ \t]*)(?=\S)
// Every piece before the operator is forced (taking less leaves a byte no later piece can take), so the only choices
// are the operator alternatives, tried in order, each followed by greedy blanks and the look-ahead.
int64_t assignment_end(const uint8_t* s, int64_t n) {
    auto name_char = [](uint8_t c) { return alnum(c) || c == '_' || c == '.' || c == '-'; };
    int64_t i = 0;
    while (i < n && blank(s[i])) ++i;
    if (i < n && (s[i] == '-' || s[i] == '+')) ++i;
    while (i < n && blank(s[i])) ++i;
    if (i < n && (s[i] == '"' || s[i] == '\'')) ++i;
    if (!(i < n && ((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z') || s[i] == '_'))) return -1;
    ++i;
    int64_t run = 0;
    while (i < n && name_char(s[i])) {
        ++i;
        ++run;
    }
    if (run > 47) return -1;
    if (i < n && (s[i] == '"' || s[i] == '\'')) ++i;
    while (i < n && blank(s[i])) ++i;
    auto value_start = [&](int64_t j) -> int64_t {
        while (j < n && blank(s[j])) ++j;
        return (j < n && !space(s[j])) ? j : -1;
    };
    int64_t r;
    if (i + 1 < n && s[i] == ':' && s[i + 1] == '=' && (r = value_start(i + 2)) >= 0) return r;
    if (i + 1 < n && s[i] == '=' && s[i + 1] == '>' && (r = value_start(i + 2)) >= 0) return r;
    if (i < n && s[i] == '=' && (r = value_start(i + 1)) >= 0) return r;
    if (i + 1 < n && s[i] == ':' && blank(s[i + 1]) && (r = value_start(i + 2)) >= 0) return r;
    return -1;
}

CodeIndex::CodeIndex(const std::vector<uint8_t>& b, const LineIndex& lines) : b_(b), lines_(lines) {
    for (int64_t i = 0; i < static_cast<int64_t>(b.size()); ++i) {
        const uint8_t c = b[static_cast<size_t>(i)];
        if (c == '"') quotes_[0].push_back(i);
        if (c == '\'') quotes_[1].push_back(i);
        if (c == '`') quotes_[2].push_back(i);
    }
}

const CodeIndex::Line& CodeIndex::line(int64_t line_start) const {
    const auto hit = cache_.find(line_start);
    if (hit != cache_.end()) return hit->second;
    const int64_t line_end = lines_.next_newline(line_start);
    Line l{assignment_end(b_.data() + line_start, line_end - line_start), {-1, -1}};
    if (l.assignment >= 0) {
        int64_t v0 = line_start + l.assignment, v1 = line_end;
        while (v1 > v0 && (b_[v1 - 1] == ' ' || b_[v1 - 1] == '\t' || b_[v1 - 1] == '\r')) --v1;
        if (v1 - v0 >= 2 && b_[v0] == b_[v1 - 1] && (b_[v0] == '"' || b_[v0] == '\'' || b_[v0] == '`')) {
            ++v0;
            --v1;
        }
        l.value = {v0, v1};
    }
    return cache_.emplace(line_start, l).first->second;
}

std::vector<Range> split_at_assignments(const CodeIndex& index, int64_t a, int64_t z) {
    std::vector<Range> chunks;
    int64_t start = a;
    const std::vector<int64_t>& newlines = index.lines().newlines();
    for (auto it = std::lower_bound(newlines.begin(), newlines.end(), a); it != newlines.end() && *it < z; ++it) {
        const int64_t i = *it;
        const int64_t value = index.assignment(i + 1);  // of the line that starts after this line feed
        if (value >= 0) {
            if (i > start) chunks.push_back({start, i});
            start = i + 1 + value;
        }
    }
    if (z > start) chunks.push_back({start, z});
    if (chunks.empty()) chunks.push_back({a, z});
    return chunks;
}

Range snap_to_literal(const CodeIndex& index, int64_t a, int64_t z, int64_t margin) {
    const int64_t line_start = index.lines().line_start(a), line_end = index.lines().next_newline(z);
    for (int kind = 0; kind < 3; ++kind) {  // ", ', `
        const std::vector<int64_t>& at = index.quotes(kind);
        const auto open = std::lower_bound(at.begin(), at.end(), a);  // the last quote before a, if on the line
        if (open == at.begin() || *(open - 1) < line_start) continue;
        const auto close = std::lower_bound(at.begin(), at.end(), z);  // the first at or after z, if on the line
        if (close == at.end() || *close >= line_end || *close - *(open - 1) > margin) continue;
        // Quotes before the opening one on the line: an even count means it really opens.
        const auto first_on_line = std::lower_bound(at.begin(), at.end(), line_start);
        if ((open - 1 - first_on_line) % 2 == 0) return {*(open - 1) + 1, *close};
    }
    return {a, z};
}

Range snap_to_value(const CodeIndex& index, int64_t a, int64_t z) {
    if (index.lines().next_newline(a) < z) return {a, z};  // the span crosses a line
    const Range v = index.value(index.lines().line_start(a));
    if (v.first >= 0 && v.first <= a && z <= v.second && (v.second - v.first) <= (z - a) + 40) return v;
    return {a, z};
}

Range line_around(const std::vector<uint8_t>& b, int64_t a, int64_t z, int64_t maximum) {
    // The line is longer than `maximum` as soon as the span is, or no line feed lies within `maximum` bytes around it:
    // the search never reads further.
    const int64_t n = static_cast<int64_t>(b.size());
    if (z - a > maximum) return {a, z};
    const int64_t floor = std::max<int64_t>(0, a - maximum - 1);
    const int64_t before = rfind_byte(b, '\n', floor, a);
    if (before < 0 && floor > 0) return {a, z};
    const int64_t line_start = before + 1;
    const int64_t ceiling = std::min(n, line_start + maximum + 1);
    const int64_t after = find_byte(b, '\n', z, ceiling);
    if (after < 0 && ceiling < n) return {a, z};
    int64_t line_end = after >= 0 ? after : n;
    if (line_end - line_start > maximum) return {a, z};
    while (line_end > line_start && (b[line_end - 1] == ' ' || b[line_end - 1] == '\t' || b[line_end - 1] == '\r'))
        --line_end;
    return (line_start <= a && z <= line_end) ? Range{line_start, line_end} : Range{a, z};
}

Range extend_to_block(const std::vector<Range>& blocks, int64_t a, int64_t z) {
    // Sorted and disjoint: the first block that ends after a is the only one that can be the first to meet [a, z).
    const auto s =
        std::upper_bound(blocks.begin(), blocks.end(), a, [](int64_t x, const Range& r) { return x < r.second; });
    if (s != blocks.end() && s->first < z) return {std::min(a, s->first), std::max(z, s->second)};
    return {a, z};
}

bool inside_one_of(const std::vector<Range>& ranges, int64_t a, int64_t z) {
    // Sorted and disjoint: only the last range that starts at or before a can hold [a, z).
    const auto r =
        std::upper_bound(ranges.begin(), ranges.end(), a, [](int64_t x, const Range& q) { return x < q.first; });
    return r != ranges.begin() && z <= (r - 1)->second;
}

bool is_documentation_example(const std::vector<uint8_t>& b, int64_t a, int64_t z) {
    // The example credentials of the AWS and Stripe documentation (split so that no scanner mistakes this file for
    // one that holds keys).
    // Public documentation examples, which are what this rule looks for (`purebyte:allow`: not findings).
    static const std::string kExamples[] = {
        std::string("AKIAIOSFODNN7") + "EXAMPLE",                      // purebyte:allow
        std::string("wJalrXUtnFEMI/K7MDENG/bPxRfiCY") + "EXAMPLEKEY",  // purebyte:allow
        std::string("AKIAI44QH8DHB") + "EXAMPLE",                      // purebyte:allow
        std::string("je7MtGbClwBF/2Zp9Utk/h3yCo8nvb") + "EXAMPLEKEY",  // purebyte:allow
        std::string("sk_") + "test_" + "4eC39HqLyjWDarjtT1zdp7dc",     // purebyte:allow
        std::string("pk_") + "test_" + "TYooMQauvdEDq54NiTphI7jx",     // purebyte:allow
    };
    const int64_t lo = std::max<int64_t>(0, a - 4), hi = std::min<int64_t>(static_cast<int64_t>(b.size()), z + 4);
    if (hi <= lo) return false;
    for (const std::string& example : kExamples) {
        if (static_cast<int64_t>(example.size()) > hi - lo) continue;
        if (std::search(b.begin() + lo, b.begin() + hi, example.begin(), example.end()) != b.begin() + hi) return true;
    }
    return false;
}

}  // namespace pb::detect
