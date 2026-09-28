// The masked and clear views of findings (detect/masking.h, detect/text.h): overlapping findings never leave a byte in
// clear, and the context of a finding is its line, bounded on long lines so that a document of one huge line costs no
// copy of it per finding. Ordinary lines give what they always gave. And a document with 10^5 findings costs
// O(F log F) to analyze and to show.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/utf8.h"
#include "crafted_models.h"
#include "detect/common.h"
#include "detect/masking.h"
#include "detect/profile.h"
#include "detect/text.h"
#include "model/model.h"
#include "runtime/session.h"
#include "test.h"

using pb::detect::context_range;
using pb::detect::Finding;
using pb::detect::kContextMarginBytes;
using pb::detect::kContextSpanBytes;
using pb::detect::LineIndex;
using pb::detect::Range;

namespace {

using Bytes = std::vector<uint8_t>;

Bytes bytes_of(const std::string& s) { return Bytes(s.begin(), s.end()); }

Finding text_finding(const Bytes& b, int64_t start, int64_t end) {
    Finding f;
    f.start = start;
    f.end = end;
    f.text = pb::detect::text_of(b, start, end);
    return f;
}

// True when no run of 3 bytes of [a, z) of `b` appears in `shown`.
bool nothing_of(const Bytes& b, int64_t a, int64_t z, const std::string& shown) {
    for (int64_t i = a; i + 3 <= z; ++i)
        if (shown.find(std::string(b.begin() + i, b.begin() + i + 3)) != std::string::npos) return false;
    return true;
}

// strip() as it was first written (a table of every code point), the reference of the one that needs no table.
bool reference_space(uint32_t c) {
    return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}
uint32_t reference_decode(const std::string& s, size_t i, size_t* length) {
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
std::string reference_strip(const std::string& s) {
    std::vector<std::pair<size_t, size_t>> points;
    for (size_t i = 0, n; i < s.size(); i += n) {
        reference_decode(s, i, &n);
        points.push_back({i, n});
    }
    auto space = [&](size_t k) {
        size_t n;
        return reference_space(reference_decode(s, points[k].first, &n));
    };
    size_t lo = 0, hi = points.size();
    while (lo < hi && space(lo)) ++lo;
    while (hi > lo && space(hi - 1)) --hi;
    if (lo >= hi) return std::string();
    return s.substr(points[lo].first, points[hi - 1].first + points[hi - 1].second - points[lo].first);
}

// Random UTF-8 text: letters, spaces of every kind, multi-byte characters and, when `line` > 0, a newline every `line`
// code points on average.
std::string random_text(pbtest::Random& rng, size_t points, int line) {
    static const uint32_t pool[] = {'a',    'b',    'Z',  '7',  '=',    '"',    ' ',     '\t',   0xA0,
                                    0x2003, 0x3000, 0x85, 0xE9, 0x4E2D, 0x200B, 0x1F600, 0xFEFF, 0x205F};
    std::string s;
    for (size_t i = 0; i < points; ++i) {
        if (line > 0 && rng.below(line) == 0)
            s += '\n';
        else
            pb::utf8::append(s, pool[rng.below(static_cast<int>(sizeof pool / sizeof pool[0]))]);
    }
    return s;
}

}  // namespace

TEST(masking_overlapping_findings_leave_nothing_in_clear) {
    // Two findings of two ensemble members: [7, 27) and [17, 47). Masked one after the other, the second one's bytes
    // after the first one's end ("klmnopqrstuvwxyzAB") were shown in clear.
    const std::string line = "token: 0123456789abcdefghijklmnopqrstuvwxyzABCD end";
    const Bytes b = bytes_of(line);
    const Finding first = text_finding(b, 7, 27), second = text_finding(b, 17, 47);
    struct Case {
        std::vector<Finding> others;
        Finding finding;
    };
    // `others` holds the findings of the document (the finding itself among them, or not: per_model output).
    for (const Case& c : {Case{{first, second}, first}, Case{{first, second}, second}, Case{{second, first}, first},
                          Case{{second, first}, second}, Case{{first}, second}, Case{{second}, first}}) {
        const std::string shown = pb::detect::masked_line(c.others, c.finding, b);
        CHECK_MSG(nothing_of(b, 11, 45, shown), shown);
        CHECK(shown.rfind("token: ", 0) == 0 && shown.size() > 4 && shown.compare(shown.size() - 4, 4, " end") == 0);
    }

    // A finding inside another one; and findings in different tokens keep the text between them.
    const Finding outer = text_finding(b, 7, 47), inner = text_finding(b, 20, 30);
    CHECK(nothing_of(b, 11, 45, pb::detect::masked_line({outer, inner}, inner, b)));
    const Bytes two_tokens = bytes_of("id=0123456789abcdefgh then key=ijklmnopqrstuvwxyz99 end");
    const Finding value1 = text_finding(two_tokens, 3, 21), value2 = text_finding(two_tokens, 31, 51);
    const std::string apart = pb::detect::masked_line({value1, value2}, value2, two_tokens);
    CHECK_MSG(nothing_of(two_tokens, 7, 19, apart) && nothing_of(two_tokens, 35, 49, apart), apart);
    CHECK_MSG(apart.find(" then key=") != std::string::npos && apart.find(" end") != std::string::npos, apart);

    // The printable string around binary findings: the same rule.
    Finding one = first, two = second;
    for (Finding* f : {&one, &two}) {
        f->binary = true;
        f->string_start = 0;
        f->string_end = static_cast<int64_t>(b.size());
    }
    for (const Finding& f : {one, two}) CHECK(nothing_of(b, 11, 45, pb::detect::masked_string({one, two}, f, b)));
}

TEST(masking_context_of_long_lines_is_bounded) {
    // One line of 1 MiB with a key-like token every 1000 bytes: each finding's context is the part of the line around
    // it, not a copy of the line.
    pbtest::Random rng(11);
    std::string doc(1u << 20, '.');
    for (size_t at = 500; at + 30 < doc.size(); at += 1000)
        for (size_t i = 0; i < 24; ++i) doc[at + i] = static_cast<char>('A' + rng.below(26));
    const Bytes b = bytes_of(doc);
    const LineIndex lines(b);
    std::vector<Finding> findings;
    for (int64_t at = 500; at + 30 < static_cast<int64_t>(b.size()); at += 1000) {
        Finding f;
        f.start = at;
        f.end = at + 24;
        pb::detect::locate(f, b, lines);
        findings.push_back(f);
    }
    CHECK(findings.size() > 1000);
    for (size_t k = 0; k < findings.size(); k += 97) {
        const Finding& f = findings[k];
        CHECK(f.line == 1 && f.col == f.start + 1);
        const int64_t lo = std::max<int64_t>(0, f.start - kContextMarginBytes), hi = f.end + kContextMarginBytes;
        CHECK(f.context == doc.substr(static_cast<size_t>(lo), static_cast<size_t>(hi - lo)));
        const std::string shown = pb::detect::masked_line(findings, f, b);
        CHECK(pb::utf8::count_code_points(shown) <= 161);
        CHECK(nothing_of(b, f.start + 4, f.end - 2, shown));
    }

    // A huge finding (a run of 300 KB of base64-like text) counts for at most kContextSpanBytes of its line.
    const Bytes run = bytes_of("k=" + std::string(300000, 'Q') + " tail");
    Finding big;
    big.start = 2;
    big.end = 2 + 300000;
    pb::detect::locate(big, run, LineIndex(run));
    CHECK(big.context.size() == static_cast<size_t>(2 + kContextSpanBytes + kContextMarginBytes));
    CHECK(pb::detect::masked_line({big}, big, run).size() < 64);
}

TEST(masking_context_of_ordinary_lines_is_the_line) {
    // Random documents: on a line that fits the bounds, the context is the whole line, as it always was; on a longer
    // one it is cut within the bounds, at character boundaries. Searching for the line gives the same range as
    // knowing it.
    pbtest::Random rng(3);
    for (int doc = 0; doc < 60; ++doc) {
        const std::string text = random_text(rng, doc % 3 == 0 ? 3000 : 400, doc % 3 == 0 ? 0 : 40);
        const Bytes b = bytes_of(text);
        const LineIndex lines(b);
        for (int k = 0; k < 40; ++k) {
            const int64_t a = rng.below(static_cast<int>(b.size()));
            const int64_t z = std::min<int64_t>(static_cast<int64_t>(b.size()), a + 1 + rng.below(64));
            int64_t line, col, line_start, line_end;
            lines.locate(a, line, col, line_start, line_end);
            const Range known = context_range(b, a, z, line_start, line_end);
            const Range searched = context_range(b, a, z);
            CHECK(known == searched);
            const bool fits = a - line_start <= kContextMarginBytes &&
                              line_end <= std::min(z, a + kContextSpanBytes) + kContextMarginBytes;
            if (fits) {
                CHECK(known.first == line_start && known.second == line_end);
                Finding f;
                f.start = a;
                f.end = z;
                pb::detect::locate(f, b, lines);
                CHECK(f.context == pb::detect::strip(pb::detect::text_of(b, line_start, line_end)));
            } else {
                CHECK(known.first >= line_start && known.first <= a && known.second >= a && known.second <= line_end);
                CHECK(a - known.first <= kContextMarginBytes);
                CHECK(known.second - a <= kContextSpanBytes + kContextMarginBytes + 64);
                CHECK(known.first == line_start || (b[static_cast<size_t>(known.first)] & 0xC0) != 0x80);
                CHECK(known.second == line_end || (b[static_cast<size_t>(known.second)] & 0xC0) != 0x80);
            }
        }
    }
}

TEST(masking_strip_is_unchanged) {
    pbtest::Random rng(7);
    for (int i = 0; i < 3000; ++i) {
        const std::string s = random_text(rng, static_cast<size_t>(rng.below(24)), i % 2 == 0 ? 6 : 0);
        CHECK(pb::detect::strip(s) == reference_strip(s));
    }
    CHECK(pb::detect::strip("") == "");
    CHECK(pb::detect::strip(" \t\xC2\xA0\xE3\x80\x80") == "");
    CHECK(pb::detect::strip("\xE2\x80\x83 key = value \xC2\x85") == "key = value");
}

TEST(masking_widens_partial_tokens) {
    // A finding that covers only the first segment of a JWT: the rest of the token (payload and signature) must not be
    // shown next to the mask, in the context line of a text finding nor in the printable string of a binary one.
    const std::string line = "Authorization: Bearer eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiI0MiJ9.c2lnbmF0dXJlLWRlbW8x end";
    const Bytes b = bytes_of(line);
    const int64_t token = static_cast<int64_t>(line.find("eyJ")), header_end = static_cast<int64_t>(line.find('.'));
    Finding f = text_finding(b, token, header_end);
    // The mask keeps the first 4 and the last 2 characters of what it hides; nothing else of the token may appear.
    auto rest_hidden = [](const std::string& s) {
        return s.find("eyJzdWIi") == std::string::npos && s.find("c2lnbmF0dXJl") == std::string::npos &&
               s.find("IUzI1NiJ9") == std::string::npos;
    };
    const std::string shown = pb::detect::masked_line({f}, f, b);
    CHECK_MSG(rest_hidden(shown), shown);
    CHECK_MSG(shown.rfind("Authorization: Bearer ", 0) == 0 && shown.find(" end") != std::string::npos, shown);
    f.binary = true;
    f.string_start = 0;
    f.string_end = static_cast<int64_t>(b.size());
    const std::string string_shown = pb::detect::masked_string({f}, f, b);
    CHECK_MSG(rest_hidden(string_shown), string_shown);
    // The name of a `key=value` stays readable when the finding covers part of the value.
    const Bytes kv = bytes_of("client_secret=Zq7vN2mKx8Rb4TtW9pLc3YhF6sJd1QgA==");
    const std::string kv_shown = pb::detect::masked_line({text_finding(kv, 20, 40)}, text_finding(kv, 20, 40), kv);
    CHECK_MSG(kv_shown.rfind("client_secret=", 0) == 0 && kv_shown.find("Kx8Rb4") == std::string::npos, kv_shown);
    // The value itself is masked as before: only the finding's own bytes.
    CHECK(pb::detect::mask(f.text) == pb::detect::mask(std::string(line.begin() + token, line.begin() + header_end)));
}

TEST(masking_pem_headers_shown_are_key_types_only) {
    // A value whose first line looks like a PEM header is shown as it is only when its label names a key type
    // (review R1, finding 26): any other label may hold the value itself.
    using pb::detect::mask;
    CHECK(mask("-----BEGIN RSA PRIVATE KEY-----\nMIIEow...\n") ==
          "-----BEGIN RSA PRIVATE KEY----- \xE2\x80\xA6 (3 lines)");
    CHECK(mask("-----BEGIN OPENSSH PRIVATE KEY-----") == "-----BEGIN OPENSSH PRIVATE KEY-----");
    CHECK(mask("-----BEGIN PGP PUBLIC KEY BLOCK-----") == "-----BEGIN PGP PUBLIC KEY BLOCK-----");
    const std::string label = std::string("AKIA") + "QWERTYUIOPASDFGH";
    for (const std::string& line : {"-----BEGIN " + label + " KEY-----", std::string("-----BEGIN CERTIFICATE-----"),
                                    "-----BEGIN " + label + "-----"})
        CHECK_MSG(mask(line).find("QWERTY") == std::string::npos && mask(line).find('*') != std::string::npos, line);
}

TEST(masking_many_findings_cost_little) {
    // 10^5 findings in one document, one per line and all on one line, found by an ensemble of three members: every
    // profile's rules and ensemble vote, and the masked view of every finding, cost O(F log F) (review R1, finding 5).
    // The quadratic versions took minutes (none, secrets-code) to hours (secrets-binary on one line) here.
    std::string lines, one_line;
    for (int i = 0; i < 100000; ++i) {
        lines += "x A y\n";
        one_line += "x A y ";
    }
    const std::vector<uint8_t> file = pbtest::flag_model(pbtest::is_upper);
    const auto model = pb::load_model_memory(file.data(), file.size());
    pb::Session session(4, 0, "auto");
    pb::detect::DetectRequest request;
    request.models = {model.get(), model.get(), model.get()};
    for (const pb::detect::Profile* profile :
         {&pb::detect::none_profile(), &pb::detect::secrets_code_profile(), &pb::detect::secrets_binary_profile()})
        for (const std::string* text : {&lines, &one_line}) {
            const auto start = std::chrono::steady_clock::now();
            std::vector<pb::detect::Document> docs{{"input", Bytes(text->begin(), text->end())}};
            const std::vector<pb::detect::DocumentResult> results = profile->run(session, request, docs);
            const pb::detect::DocumentResult& r = results.at(0);
            const pb::detect::MaskedViews views(docs[0].bytes, r.findings, &r.hidden);
            size_t shown = 0;
            for (const Finding& f : r.findings) shown += (f.binary ? views.string(f) : views.line(f)).size();
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            const std::string what = std::string(profile->name()) + (text == &lines ? ", lines" : ", one line");
            CHECK_MSG(seconds < 30.0, what + ": " + std::to_string(seconds) + " s");
            // secrets-binary reports a string equal to the previous finding's less than 512 bytes after it as the
            // same finding: one every 86 spans of 6 bytes, 1,163 in all.
            const size_t expected = std::string(profile->name()) == "secrets-binary" ? 1163 : 100000;
            CHECK_MSG(r.findings.size() == expected, what + ": " + std::to_string(r.findings.size()) + " findings");
            CHECK(shown > 0);
            for (size_t k = 0; k < r.findings.size(); k += 997) CHECK(r.findings[k].votes == 3);
        }
}
