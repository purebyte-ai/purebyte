#include "detect/redaction.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <utility>

#include "core/digest.h"
#include "core/failure.h"
#include "core/json.h"
#include "core/utf8.h"
#include "detect/spans.h"

namespace pb::detect {

namespace {

// ----------------------------------------------------------------------------------------------- normalisation
// Values are compared as code points; bytes that are not valid UTF-8 stay distinct (U+DC80 + byte, as Python's
// "surrogateescape"), so two different invalid values never share a marker.
std::vector<uint32_t> code_points(const uint8_t* p, size_t n) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < n;) {
        const uint8_t c = p[i];
        size_t len = c < 0x80                ? 1
                     : c >= 0xC2 && c < 0xE0 ? 2
                     : c >= 0xE0 && c < 0xF0 ? 3
                     : c >= 0xF0 && c < 0xF5 ? 4
                                             : 0;
        const std::string piece = len && i + len <= n ? utf8::decode_replace(p + i, len) : std::string();
        if (len && i + len <= n && piece.find("\xEF\xBF\xBD") == std::string::npos) {
            uint32_t cp = len == 1 ? c : c & (0xFF >> (len + 1));
            for (size_t k = 1; k < len; ++k) cp = (cp << 6) | (p[i + k] & 0x3F);
            out.push_back(cp);
            i += len;
        } else {
            out.push_back(0xDC00 + c);
            ++i;
        }
    }
    return out;
}

std::string to_utf8(const std::vector<uint32_t>& cps) {
    std::string out;
    for (uint32_t cp : cps) utf8::append(out, cp);
    return out;
}

bool is_space(uint32_t c) {
    return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

bool is_digit(uint32_t c) {
    return (c >= '0' && c <= '9') || (c >= 0x0660 && c <= 0x0669) || (c >= 0x06F0 && c <= 0x06F9) ||
           (c >= 0x0966 && c <= 0x096F) || (c >= 0xFF10 && c <= 0xFF19);
}

// Simple case mapping of the scripts personal data is usually written in (Latin, Greek, Cyrillic).
uint32_t lower(uint32_t c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7) || (c >= 0x391 && c <= 0x3AB && c != 0x3A2) ||
        (c >= 0x410 && c <= 0x42F))
        return c + 0x20;
    if (c >= 0x400 && c <= 0x40F) return c + 0x50;
    if (c == 0x178) return 0xFF;
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return c | 1;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c + 1 : c;
    return c;
}

uint32_t upper(uint32_t c) {
    if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7) || (c >= 0x3B1 && c <= 0x3CB && c != 0x3C2) ||
        (c >= 0x430 && c <= 0x44F))
        return c - 0x20;
    if (c >= 0x450 && c <= 0x45F) return c - 0x50;
    if (c == 0xFF) return 0x178;
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return c & ~1u;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c : c - 1;
    return c;
}

// The compatibility decomposition to ASCII of a code point of U+0080..U+017F (NFKD, then "ascii" with errors
// ignored), for the Latin letters names and addresses use. False when it has none: the caller decides what to keep.
bool latin_ascii(uint32_t c, std::string& out) {
    // U+00C0..U+00FF and U+0100..U+017F: the base letter, or '-' when the character has no ASCII decomposition.
    static const char kLatin1[] =
        "AAAAAA-CEEEEIIII-NOOOOO--UUUUY--"   // U+00C0..U+00DF
        "aaaaaa-ceeeeiiii-nooooo--uuuuy-y";  // U+00E0..U+00FF
    static const char kLatinA[] =
        "AaAaAaCcCcCcCcDd--EeEeEeEeEeGgGgGgGgHh--"  // U+0100..U+0127
        "IiIiIiIiI---JjKk-LlLlLlLl--NnNnNnn--"      // U+0128..U+014B (U+0132, U+0133 are handled apart)
        "OoOoOo--RrRrRrSsSsSsSsTtTt--"              // U+014C..U+0167
        "UuUuUuUuUuUuWwYyYZzZzZzs";                 // U+0168..U+017F
    static_assert(sizeof kLatin1 == 64 + 1 && sizeof kLatinA == 128 + 1, "one entry per code point");
    char letter = '-';
    if (c == 0xA0)
        letter = ' ';
    else if (c == 0xAA)
        letter = 'a';
    else if (c == 0xBA)
        letter = 'o';
    else if (c == 0xB2 || c == 0xB3)
        letter = static_cast<char>('0' + (c - 0xB0));
    else if (c == 0xB9)
        letter = '1';
    else if (c == 0x132 || c == 0x133) {
        out = c == 0x132 ? "IJ" : "ij";
        return true;
    } else if (c >= 0xC0 && c <= 0xFF)
        letter = kLatin1[c - 0xC0];
    else if (c >= 0x100 && c <= 0x17F)
        letter = kLatinA[c - 0x100];
    if (letter == '-') return false;
    out.assign(1, letter);
    return true;
}

// Greek and Cyrillic letters with an accent (tonos, dialytika, breve...): the letter without it, as NFKD and the
// removal of combining marks give it (final sigma becomes sigma, as case folding does). Other code points unchanged.
uint32_t base_letter(uint32_t c) {
    static const uint32_t kPairs[][2] = {
        {0x386, 0x391}, {0x388, 0x395}, {0x389, 0x397}, {0x38A, 0x399}, {0x38C, 0x39F}, {0x38E, 0x3A5}, {0x38F, 0x3A9},
        {0x390, 0x3B9}, {0x3AA, 0x399}, {0x3AB, 0x3A5}, {0x3AC, 0x3B1}, {0x3AD, 0x3B5}, {0x3AE, 0x3B7}, {0x3AF, 0x3B9},
        {0x3B0, 0x3C5}, {0x3C2, 0x3C3}, {0x3CA, 0x3B9}, {0x3CB, 0x3C5}, {0x3CC, 0x3BF}, {0x3CD, 0x3C5}, {0x3CE, 0x3C9},
        {0x400, 0x415}, {0x401, 0x415}, {0x403, 0x413}, {0x407, 0x406}, {0x40C, 0x41A}, {0x40D, 0x418}, {0x40E, 0x423},
        {0x419, 0x418}, {0x439, 0x438}, {0x450, 0x435}, {0x451, 0x435}, {0x453, 0x433}, {0x457, 0x456}, {0x45C, 0x43A},
        {0x45D, 0x438}, {0x45E, 0x443}};
    for (const auto& p : kPairs)
        if (p[0] == c) return p[1];
    return c;
}

// Combining marks (what an accent becomes after NFKD) and invisible format characters: never part of what makes two
// names different.
bool ignorable(uint32_t c) {
    return (c >= 0x300 && c <= 0x36F) || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x1DC0 && c <= 0x1DFF) ||
           (c >= 0x20D0 && c <= 0x20FF) || (c >= 0xFE20 && c <= 0xFE2F) || c == 0xAD || (c >= 0x200B && c <= 0x200F) ||
           (c >= 0x202A && c <= 0x202E) || (c >= 0x2060 && c <= 0x2064) || c == 0xFEFF;
}

// A name or an address as markers compare it: Latin letters without their accents (their ASCII decomposition), every
// other letter kept (Cyrillic, Greek, CJK, Arabic... Greek and Cyrillic ones without their accents), the Latin-1
// symbols without a decomposition, combining marks and invisible characters dropped. Then lower case, and every run
// of white space as one space, trimmed.
std::vector<uint32_t> fold_name(const std::vector<uint32_t>& cps) {
    std::vector<uint32_t> folded;
    std::string ascii;
    for (uint32_t c : cps) {
        if (c < 0x80) {
            folded.push_back(c);
        } else if (latin_ascii(c, ascii)) {
            for (char a : ascii) folded.push_back(static_cast<unsigned char>(a));
        } else if (c < 0xC0 || c == 0xD7 || c == 0xF7 || ignorable(c)) {
            continue;  // C1 controls, Latin-1 punctuation and signs (x, /), marks: dropped as before
        } else {
            folded.push_back(base_letter(c));
        }
    }
    std::vector<uint32_t> out;
    for (size_t i = 0; i < folded.size();) {
        if (is_space(folded[i])) {
            while (i < folded.size() && is_space(folded[i])) ++i;
            if (!out.empty()) out.push_back(' ');
        } else {
            out.push_back(lower(folded[i++]));
        }
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// The value 0..9 of a decimal digit (ASCII, Arabic-Indic, extended Arabic-Indic, Devanagari, fullwidth), else -1.
int digit_value(uint32_t c) {
    if (!is_digit(c)) return -1;
    for (uint32_t zero : {0x30u, 0x660u, 0x6F0u, 0x966u, 0xFF10u})
        if (c >= zero && c <= zero + 9) return static_cast<int>(c - zero);
    return -1;
}

// Digits of the E.164 country code at `from` of `digits`: 1 for 1 (North America) and 7, 2 for the two-digit codes,
// 3 otherwise (the codes are prefix-free); 0 when fewer digits remain than the code needs.
size_t country_code_digits(const std::vector<int>& digits, size_t from) {
    static const int kTwoDigit[] = {20, 27, 30, 31, 32, 33, 34, 36, 39, 40, 41, 43, 44, 45, 46,
                                    47, 48, 49, 51, 52, 53, 54, 55, 56, 57, 58, 60, 61, 62, 63,
                                    64, 65, 66, 81, 82, 84, 86, 90, 91, 92, 93, 94, 95, 98};
    if (from >= digits.size()) return 0;
    if (digits[from] == 1 || digits[from] == 7) return 1;
    if (from + 1 >= digits.size()) return 0;
    const int two = digits[from] * 10 + digits[from + 1];
    for (int code : kTwoDigit)
        if (code == two) return 2;
    return from + 2 < digits.size() ? 3 : 0;
}

// [first, last) of `v` without leading and trailing white space.
std::pair<size_t, size_t> stripped(const std::vector<uint32_t>& v) {
    size_t first = 0, last = v.size();
    while (first < last && is_space(v[first])) ++first;
    while (last > first && is_space(v[last - 1])) --last;
    return {first, last};
}

// -------------------------------------------------------------------------------------------------- markers
// The highest n of every [TYPE_n] already in the input, for the known types (the first type that matches at a
// position wins, and matches do not overlap).
std::map<std::string, int64_t> existing_markers(const std::vector<uint8_t>& data,
                                                const std::vector<std::string>& types) {
    std::map<std::string, int64_t> top;
    const size_t n = data.size();
    for (size_t i = 0; i < n; ++i) {
        if (data[i] != '[') continue;
        for (const std::string& t : types) {
            size_t j = i + 1;
            if (j + t.size() > n || std::memcmp(data.data() + j, t.data(), t.size()) != 0) continue;
            j += t.size();
            if (j >= n || data[j] != '_') continue;
            ++j;
            const size_t digits = j;
            int64_t value = 0;
            while (j < n && data[j] >= '0' && data[j] <= '9') {
                if (value < 100000000000000LL) value = value * 10 + (data[j] - '0');
                ++j;
            }
            if (j == digits || j >= n || data[j] != ']') continue;
            top[t] = std::max(top[t], value);
            i = j;
            break;
        }
    }
    return top;
}

// The copy property: every input byte outside the spans is in the output, in order and unchanged, and the output
// holds nothing else than the markers at their reported places.
struct Entry {
    std::string type, marker, source;
    int64_t start, end, out_start, out_end;
    float confidence;
    std::string marker_bytes;  // the marker as the output holds it (in UTF-16 for a UTF-16 document)
};

void check_copy(const std::vector<uint8_t>& in, const std::vector<uint8_t>& out, const std::vector<Entry>& entries) {
    const int64_t n = static_cast<int64_t>(in.size()), m = static_cast<int64_t>(out.size());
    int64_t i = 0, j = 0;
    for (const Entry& e : entries) {
        const int64_t gap = e.start - i;
        if (gap < 0 || e.end < e.start || e.end > n || j + gap != e.out_start || e.out_start > m ||
            !std::equal(in.begin() + i, in.begin() + e.start, out.begin() + j))
            fail(PB_ERR_INTERNAL,
                 format("redaction broke the copy property before input byte %lld", static_cast<long long>(e.start)));
        if (e.out_end - e.out_start != static_cast<int64_t>(e.marker_bytes.size()) || e.out_end > m ||
            !std::equal(e.marker_bytes.begin(), e.marker_bytes.end(), out.begin() + e.out_start))
            fail(PB_ERR_INTERNAL, "redaction misplaced a marker");
        i = e.end;
        j = e.out_end;
    }
    if (static_cast<int64_t>(in.size()) - i != static_cast<int64_t>(out.size()) - j ||
        !std::equal(in.begin() + i, in.end(), out.begin() + j))
        fail(PB_ERR_INTERNAL, "redaction broke the copy property after the last span");
}

// round(x, 4) as a decimal: the double nearest to x rounded to 4 decimals (ties to even), the value "%.4f" prints.
// x * 10000 is exact in double (24 + 14 bits), so this is exact arithmetic, the same under every locale.
double round4(float x) { return std::nearbyint(static_cast<double>(x) * 10000.0) / 10000.0; }

}  // namespace

std::string normalize_value(const std::string& type, const uint8_t* value, size_t size) {
    std::vector<uint32_t> s = code_points(value, size);
    std::vector<uint32_t> out;
    if (type == "EMAIL") {
        const std::pair<size_t, size_t> kept = stripped(s);
        for (size_t i = kept.first; i < kept.second; ++i) out.push_back(lower(s[i]));
    } else if (type == "PHONE") {
        // Every digit counts (two numbers that differ in any digit are two numbers); only an international prefix
        // is dropped: '+' before the first digit, or "00", then the E.164 country code. "+34 612 345 678",
        // "0034 612345678" and "612 345 678" are one number; "212-555-0123" and "312-555-0123" are two.
        std::vector<int> values;
        bool international = false;
        for (uint32_t c : s) {
            const int v = digit_value(c);
            if (v >= 0) {
                out.push_back(c);
                values.push_back(v);
            } else if (values.empty() && (c == '+' || c == 0xFF0B)) {
                international = true;
            }
        }
        size_t skip = 0;
        if (!international && values.size() >= 2 && values[0] == 0 && values[1] == 0) {
            international = true;
            skip = 2;
        }
        if (international) skip += country_code_digits(values, skip);
        out.erase(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(std::min(skip, out.size())));
    } else if (type == "IBAN" || type == "CREDIT_CARD" || type == "DNI_NIE" || type == "PASSPORT") {
        for (uint32_t c : s)
            if (!is_space(c) && c != '.' && c != '-') out.push_back(upper(c));
    } else if (type == "IP") {
        size_t a = 0, z = s.size();
        while (a < z && (s[a] == '[' || s[a] == ']')) ++a;
        while (z > a && (s[z - 1] == '[' || s[z - 1] == ']')) --z;
        for (size_t i = a; i < z; ++i) out.push_back(lower(s[i]));
    } else if (type == "PERSON_NAME" || type == "ADDRESS") {
        out = fold_name(s);
    } else {
        out = s;
    }
    return to_utf8(out);
}

namespace {

// The resolved spans, their markers, and the input with every span replaced by its marker.
struct Marked {
    std::vector<uint8_t> output;
    std::vector<Entry> entries;
};

Marked mark(const std::vector<uint8_t>& input, const std::vector<RedactionSpan>& spans,
            const RedactionRequest& request) {
    // Types by index: the model's order first, then any other type in order of appearance.
    std::vector<std::string> types = request.types;
    std::vector<Candidate> candidates;
    for (const RedactionSpan& s : spans) {
        if (s.start < 0 || s.start >= s.end || s.end > static_cast<int64_t>(input.size())) continue;
        auto it = std::find(types.begin(), types.end(), s.type);
        if (it == types.end()) it = types.insert(types.end(), s.type);
        candidates.push_back({s.start, s.end, static_cast<int32_t>(it - types.begin()), s.confidence, s.source});
    }
    const std::vector<Candidate> resolved = resolve(std::move(candidates), types);

    std::map<std::string, int64_t> count = existing_markers(input, types);
    std::map<std::pair<std::string, std::string>, std::string> markers;
    Marked m;
    int64_t last = 0;
    for (const Candidate& c : resolved) {
        const std::string& type = types[c.type];
        const auto key =
            std::make_pair(type, normalize_value(type, input.data() + c.start, static_cast<size_t>(c.end - c.start)));
        auto it = markers.find(key);
        if (it == markers.end())
            it = markers.emplace(key, "[" + type + "_" + std::to_string(++count[type]) + "]").first;
        const std::string& marker = it->second;
        m.output.insert(m.output.end(), input.begin() + last, input.begin() + c.start);
        const int64_t out_start = static_cast<int64_t>(m.output.size());
        m.output.insert(m.output.end(), marker.begin(), marker.end());
        m.entries.push_back({type, marker, c.source, c.start, c.end, out_start, static_cast<int64_t>(m.output.size()),
                             c.confidence, marker});
        last = c.end;
    }
    m.output.insert(m.output.end(), input.begin() + last, input.end());
    return m;
}

// The report and the map of `r.output`, the redaction of `input` (the entries in the offsets of both), after the
// check of the copy property. `encoding` names the encoding of both when it is not plain bytes ("utf-16le").
void render(const std::vector<uint8_t>& input, const std::vector<Entry>& entries, const RedactionRequest& request,
            const char* encoding, Redaction& r) {
    check_copy(input, r.output, entries);
    const std::string sha_out = sha256_hex(r.output.data(), r.output.size());
    json::Writer w;
    w.begin_object().field("input", request.input_name);
    if (!request.model_name.empty()) w.field("model", request.model_name);
    w.field("bytes_in", static_cast<int64_t>(input.size()))
        .field("bytes_out", static_cast<int64_t>(r.output.size()))
        .field("sha256_in", sha256_hex(input.data(), input.size()))
        .field("sha256_out", sha_out);
    if (encoding) w.field("encoding", encoding);
    std::map<std::string, int64_t> counts;
    w.key("spans").begin_array();
    for (const Entry& e : entries) {
        ++counts[e.type];
        w.begin_object()
            .field("type", e.type)
            .field("start", e.start)
            .field("end", e.end)
            .field("marker", e.marker)
            .field("confidence", round4(e.confidence))
            .field("source", e.source)
            .field("out_start", e.out_start)
            .field("out_end", e.out_end)
            .end_object();
    }
    w.end_array().key("counts").begin_object();
    for (const auto& kv : counts) w.field(kv.first, kv.second);
    w.end_object();
    w.key("guarantee").begin_object().field("outside_spans_identical", true).field("checked", true).end_object();
    w.end_object();
    r.report_json = w.take();

    if (request.with_map) {
        json::Writer m;
        m.begin_object().field("version", 1).field("input", request.input_name).field("sha256_out", sha_out);
        if (encoding) m.field("encoding", encoding);
        m.key("spans").begin_array();
        for (const Entry& e : entries)
            m.begin_object()
                .field("marker", e.marker)
                .field("out_start", e.out_start)
                .field("out_end", e.out_end)
                .field("value_b64", base64_encode(input.data() + e.start, static_cast<size_t>(e.end - e.start)))
                .end_object();
        m.end_array().end_object();
        r.map_json = m.take();
    }
}

// ------------------------------------------------------------------------------------------- UTF-16 documents
// A profile may read a UTF-16 input (with its byte order mark) as the UTF-8 text it converts it to. The redaction is
// by copy of the caller's bytes all the same: the spans found in the text are carried back to the UTF-16 bytes
// (whole characters), and the markers are written in UTF-16.

uint32_t unit16(const std::vector<uint8_t>& b, size_t i, bool big_endian) {
    return big_endian ? (static_cast<uint32_t>(b[i]) << 8) | b[i + 1] : b[i] | (static_cast<uint32_t>(b[i + 1]) << 8);
}

void append16(std::vector<uint8_t>& out, uint32_t unit, bool big_endian) {
    out.push_back(static_cast<uint8_t>(big_endian ? unit >> 8 : unit & 0xFF));
    out.push_back(static_cast<uint8_t>(big_endian ? unit & 0xFF : unit >> 8));
}

void append_code_point16(std::vector<uint8_t>& out, uint32_t cp, bool big_endian) {
    if (cp < 0x10000) return append16(out, cp, big_endian);
    cp -= 0x10000;
    append16(out, 0xD800 + (cp >> 10), big_endian);
    append16(out, 0xDC00 + (cp & 0x3FF), big_endian);
}

// The offsets of every code point of a UTF-16 document (after its byte order mark) in its own bytes (`own`) and in the
// UTF-8 text it converts to (`utf8`), then both ends. False when `text` is not that conversion.
struct Utf16Layout {
    bool big_endian = false;
    std::vector<int64_t> own, utf8;
};

bool layout_of(const std::vector<uint8_t>& utf16, const std::vector<uint8_t>& text, Utf16Layout& l) {
    if (utf16.size() < 2 || utf16.size() % 2 != 0) return false;
    if (utf16[0] == 0xFE && utf16[1] == 0xFF)
        l.big_endian = true;
    else if (!(utf16[0] == 0xFF && utf16[1] == 0xFE))
        return false;
    std::string again;
    for (size_t i = 2; i < utf16.size();) {
        uint32_t cp = unit16(utf16, i, l.big_endian);
        size_t bytes = 2;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (i + 3 >= utf16.size()) return false;
            const uint32_t low = unit16(utf16, i + 2, l.big_endian);
            if (low < 0xDC00 || low > 0xDFFF) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            bytes = 4;
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return false;
        }
        l.own.push_back(static_cast<int64_t>(i));
        l.utf8.push_back(static_cast<int64_t>(again.size()));
        utf8::append(again, cp);
        i += bytes;
    }
    l.own.push_back(static_cast<int64_t>(utf16.size()));
    l.utf8.push_back(static_cast<int64_t>(again.size()));
    return again.size() == text.size() && std::equal(again.begin(), again.end(), text.begin(),
                                                     [](char a, uint8_t b) { return static_cast<uint8_t>(a) == b; });
}

// Valid UTF-8 `text` in UTF-16 with a byte order mark; `offsets8` and `offsets16` receive the offset of every code
// point in both, then the ends.
std::vector<uint8_t> to_utf16(const std::vector<uint8_t>& text, bool big_endian, std::vector<int64_t>& offsets8,
                              std::vector<int64_t>& offsets16) {
    std::vector<uint8_t> out;
    append16(out, 0xFEFF, big_endian);
    for (size_t i = 0; i < text.size();) {
        const uint8_t c = text[i];
        const size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (i + n > text.size() ||
            utf8::decode_replace(text.data() + i, n) != std::string(text.begin() + i, text.begin() + i + n))
            fail(PB_ERR_INTERNAL, "the redacted text is not valid UTF-8");
        uint32_t cp = n == 1 ? c : c & (0xFF >> (n + 1));
        for (size_t k = 1; k < n; ++k) cp = (cp << 6) | (text[i + k] & 0x3F);
        offsets8.push_back(static_cast<int64_t>(i));
        offsets16.push_back(static_cast<int64_t>(out.size()));
        append_code_point16(out, cp, big_endian);
        i += n;
    }
    offsets8.push_back(static_cast<int64_t>(text.size()));
    offsets16.push_back(static_cast<int64_t>(out.size()));
    return out;
}

// The offset in `to` of the code point that starts at `x` in `from` (x must start one).
int64_t moved(const std::vector<int64_t>& from, const std::vector<int64_t>& to, int64_t x) {
    const auto it = std::lower_bound(from.begin(), from.end(), x);
    if (it == from.end() || *it != x) fail(PB_ERR_INTERNAL, "a redacted span does not start or end on a character");
    return to[static_cast<size_t>(it - from.begin())];
}

}  // namespace

Redaction redact(const std::vector<uint8_t>& input, const std::vector<RedactionSpan>& spans,
                 const RedactionRequest& request) {
    Marked m = mark(input, spans, request);
    Redaction r;
    r.output = std::move(m.output);
    render(input, m.entries, request, nullptr, r);
    return r;
}

Redaction redact_converted(const std::vector<uint8_t>& original, const std::vector<uint8_t>& text,
                           const std::vector<RedactionSpan>& spans, const RedactionRequest& request) {
    Utf16Layout l;
    if (!layout_of(original, text, l))
        fail(PB_ERR_INTERNAL, "the text the profile read is not the conversion of the UTF-16 input");
    // Spans on whole characters: a character a span touches is redacted whole, and no part of one is left next to a
    // marker (the output must be valid UTF-16).
    const int64_t n = static_cast<int64_t>(text.size());
    std::vector<RedactionSpan> whole = spans;
    for (RedactionSpan& s : whole) {
        if (s.start < 0 || s.start >= s.end || s.end > n) continue;  // ignored by mark(), as by redact()
        s.start = *(std::upper_bound(l.utf8.begin(), l.utf8.end(), s.start) - 1);
        s.end = *std::lower_bound(l.utf8.begin(), l.utf8.end(), s.end);
    }
    Marked m = mark(text, whole, request);
    std::vector<int64_t> out8, out16;
    Redaction r;
    r.output = to_utf16(m.output, l.big_endian, out8, out16);
    for (Entry& e : m.entries) {
        e.start = moved(l.utf8, l.own, e.start);
        e.end = moved(l.utf8, l.own, e.end);
        e.out_start = moved(out8, out16, e.out_start);
        e.out_end = moved(out8, out16, e.out_end);
        std::vector<uint8_t> marker;
        for (const char c : e.marker) append16(marker, static_cast<unsigned char>(c), l.big_endian);
        e.marker_bytes.assign(marker.begin(), marker.end());
    }
    render(original, m.entries, request, l.big_endian ? "utf-16be" : "utf-16le", r);
    return r;
}

std::vector<uint8_t> restore(const std::vector<uint8_t>& redacted, const std::string& map_json) {
    json::Value map;
    std::string error;
    if (!json::parse(map_json, map, error) || !map.is(json::Value::Type::Object))
        fail(PB_ERR_ARGUMENT, "the redaction map is not valid JSON: " + error);
    const json::Value* version = map.get("version");
    if (!version || !version->is(json::Value::Type::Number))
        fail(PB_ERR_ARGUMENT, "the redaction map has no `version`: it is not a map written by pb_redact");
    if (version->number != 1)
        fail(PB_ERR_UNSUPPORTED, "the redaction map has another version than 1, the only one this library reads");
    const json::Value* spans = map.get("spans");
    if (!spans || !spans->is(json::Value::Type::Array)) fail(PB_ERR_ARGUMENT, "the redaction map has no `spans` list");
    // The check that the output was not edited is not optional: a map without the output's SHA-256 is refused.
    const json::Value* sha = map.get("sha256_out");
    if (!sha || !sha->is(json::Value::Type::String))
        fail(PB_ERR_ARGUMENT, "the redaction map has no `sha256_out`: the output cannot be checked against it");
    if (sha->string != sha256_hex(redacted.data(), redacted.size()))
        fail(PB_ERR_ARGUMENT,
             "the redacted input is not the output this map was made for (its SHA-256 differs): it was edited "
             "after redaction, or the map belongs to another file");
    // An offset is an integer within the output; anything else (fractional, negative, 1e300) is refused before it
    // is converted.
    auto offset = [&](const json::Value& v) {
        const double x = v.number;
        if (!(x >= 0.0 && x <= static_cast<double>(redacted.size()) && x == std::floor(x)))
            fail(PB_ERR_ARGUMENT, "a span of the redaction map has an offset that is not an integer within the output");
        return static_cast<int64_t>(x);
    };
    // A UTF-16 document holds its markers in UTF-16 (redact_converted).
    bool utf16 = false, big_endian = false;
    if (const json::Value* encoding = map.get("encoding")) {
        if (encoding->is(json::Value::Type::String) && encoding->string == "utf-16le")
            utf16 = true;
        else if (encoding->is(json::Value::Type::String) && encoding->string == "utf-16be")
            utf16 = big_endian = true;
        else
            fail(PB_ERR_UNSUPPORTED,
                 "the redaction map names an encoding this library does not know (known: "
                 "utf-16le, utf-16be)");
    }
    struct Item {
        int64_t start, end;
        std::string held;  // the marker as the output holds it
        std::vector<uint8_t> value;
        std::string marker;
    };
    std::vector<Item> items;
    for (const json::Value& s : spans->array) {
        const json::Value *marker = s.get("marker"), *a = s.get("out_start"), *z = s.get("out_end"),
                          *v = s.get("value_b64");
        if (!marker || !a || !z || !v || !marker->is(json::Value::Type::String) || !a->is(json::Value::Type::Number) ||
            !z->is(json::Value::Type::Number) || !v->is(json::Value::Type::String))
            fail(PB_ERR_ARGUMENT, "a span of the redaction map lacks marker, out_start, out_end or value_b64");
        std::string held = marker->string;
        if (utf16) {
            std::vector<uint8_t> bytes;
            for (const char c : marker->string) append16(bytes, static_cast<unsigned char>(c), big_endian);
            held.assign(bytes.begin(), bytes.end());
        }
        Item item{offset(*a), offset(*z), held, {}, marker->string};
        if (!base64_decode(v->string, item.value)) fail(PB_ERR_ARGUMENT, "a value of the redaction map is not base64");
        items.push_back(std::move(item));
    }
    std::sort(items.begin(), items.end(), [](const Item& x, const Item& y) { return x.start < y.start; });
    std::vector<uint8_t> out;
    int64_t last = 0;
    for (const Item& it : items) {
        const bool in_place = it.start >= last && it.end <= static_cast<int64_t>(redacted.size()) &&
                              it.end - it.start == static_cast<int64_t>(it.held.size()) &&
                              std::equal(it.held.begin(), it.held.end(), redacted.begin() + it.start);
        if (!in_place)
            fail(PB_ERR_ARGUMENT,
                 format("the redacted input does not hold %s at byte %lld: it was edited after redaction",
                        it.marker.c_str(), static_cast<long long>(it.start)));
        out.insert(out.end(), redacted.begin() + last, redacted.begin() + it.start);
        out.insert(out.end(), it.value.begin(), it.value.end());
        last = it.end;
    }
    out.insert(out.end(), redacted.begin() + last, redacted.end());
    return out;
}

}  // namespace pb::detect
