// Properties of redaction by copy (pb.h: pb_redact_spans, pb_restore) on random documents and random spans, some of
// them overlapping, empty or out of range:
//   * restore(redact(x)) == x;
//   * the output is the input with every span replaced by its marker: outside the spans, byte for byte the same;
//   * the redacted region is the union of the valid spans, whatever their overlaps and types;
//   * the report never holds a redacted value; the same value gets the same marker; numbering never reuses a marker
//     already present in the input; an edited output cannot be restored.
#include <algorithm>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "core/json.h"
#include "purebyte/pb.h"
#include "test.h"

namespace {

struct Result {
    bool ok = false;
    std::string output, report, map;
};

Result redact(const std::string& doc, const std::vector<pb_redaction_span>& spans,
              const std::vector<const char*>& types) {
    Result r;
    pb_redaction* red = nullptr;
    pb_error err;
    const pb_status s = pb_redact_spans(reinterpret_cast<const uint8_t*>(doc.data()), doc.size(), spans.data(),
                                        spans.size(), types.data(), types.size(), PB_REDACT_WITH_MAP, &red, &err);
    if (s != PB_OK) {
        pbtest::report(__FILE__, __LINE__, std::string("pb_redact_spans: ") + err.message);
        return r;
    }
    size_t size = 0;
    const uint8_t* out = pb_redaction_output(red, &size);
    r.output.assign(reinterpret_cast<const char*>(out), size);
    r.report = pb_redaction_report(red);
    r.map = pb_redaction_map(red) ? pb_redaction_map(red) : "";
    r.ok = true;
    pb_redaction_free(red);
    return r;
}

bool restore(const std::string& output, const std::string& map, std::string& original) {
    uint8_t* out = nullptr;
    size_t size = 0;
    pb_error err;
    if (pb_restore(reinterpret_cast<const uint8_t*>(output.data()), output.size(), map.c_str(), &out, &size, &err) !=
        PB_OK)
        return false;
    original.assign(reinterpret_cast<const char*>(out), size);
    pb_free(out);
    return true;
}

bool is_marker(const std::string& m) {
    if (m.size() < 5 || m.front() != '[' || m.back() != ']') return false;
    const size_t underscore = m.rfind('_');
    if (underscore == std::string::npos || underscore + 1 >= m.size() - 1) return false;
    for (size_t i = underscore + 1; i + 1 < m.size(); ++i)
        if (m[i] < '0' || m[i] > '9') return false;
    return true;
}

}  // namespace

TEST(redaction_properties_on_random_documents) {
    pbtest::Random rng(21);
    const char* types[] = {"SECRET", "EMAIL", "NAME"};
    const char* sources[] = {"model", "rules"};
    for (int trial = 0; trial < 300; ++trial) {
        const int length = rng.below(260);
        std::string doc;
        for (int i = 0; i < length; ++i) doc += "abcdefghijklmnopqrstuvwxyz0123456789 \n"[rng.below(38)];
        if (trial % 7 == 0) doc += " [SECRET_2] ";
        std::vector<pb_redaction_span> spans;
        for (int k = rng.below(7); k > 0; --k) {
            pb_redaction_span s;
            s.start = rng.below(static_cast<int>(doc.size()) + 10) - 5;
            s.end = s.start + rng.below(24) - 2;
            s.type = types[rng.below(3)];
            s.confidence = static_cast<float>(rng.below(1000)) / 1000.f;
            s.source = sources[rng.below(2)];
            spans.push_back(s);
        }
        const Result r = redact(doc, spans, {types[0], types[1], types[2]});
        if (!r.ok) continue;
        std::string back;
        CHECK_MSG(restore(r.output, r.map, back) && back == doc, "round trip, trial " + std::to_string(trial));

        pb::json::Value report;
        std::string error;
        CHECK(pb::json::parse(r.report, report, error));
        const pb::json::Value* list = report.get("spans");
        CHECK(list != nullptr);
        if (!list) continue;
        // Walk the resolved spans in order: the gaps are copies, the spans are markers.
        std::vector<char> covered(doc.size(), 0);
        int64_t in = 0, out = 0;
        for (const pb::json::Value& s : list->array) {
            const auto start = static_cast<int64_t>(s.get("start")->number),
                       end = static_cast<int64_t>(s.get("end")->number);
            const auto out_start = static_cast<int64_t>(s.get("out_start")->number);
            const auto out_end = static_cast<int64_t>(s.get("out_end")->number);
            const std::string marker = s.get("marker")->string;
            CHECK(start >= in && end > start && end <= static_cast<int64_t>(doc.size()));
            CHECK(out_start - out == start - in);
            CHECK(r.output.compare(static_cast<size_t>(out), static_cast<size_t>(start - in), doc,
                                   static_cast<size_t>(in), static_cast<size_t>(start - in)) == 0);
            CHECK(r.output.substr(static_cast<size_t>(out_start), static_cast<size_t>(out_end - out_start)) == marker);
            CHECK_MSG(is_marker(marker), marker);
            for (int64_t i = start; i < end; ++i) covered[static_cast<size_t>(i)] = 1;
            in = end;
            out = out_end;
        }
        CHECK(r.output.compare(static_cast<size_t>(out), std::string::npos, doc, static_cast<size_t>(in),
                               std::string::npos) == 0);
        // The redacted region is the union of the spans that lie inside the document and are not empty.
        std::vector<char> expected(doc.size(), 0);
        for (const pb_redaction_span& s : spans)
            if (s.start >= 0 && s.end > s.start && s.end <= static_cast<int64_t>(doc.size()))
                for (int64_t i = s.start; i < s.end; ++i) expected[static_cast<size_t>(i)] = 1;
        CHECK_MSG(covered == expected, "redacted region, trial " + std::to_string(trial));
        // No value in the report (long values only: a short one may occur in the JSON by chance).
        for (const pb::json::Value& s : list->array) {
            const auto start = static_cast<size_t>(s.get("start")->number),
                       end = static_cast<size_t>(s.get("end")->number);
            if (end - start >= 10) CHECK(r.report.find(doc.substr(start, end - start)) == std::string::npos);
        }
    }
}

TEST(redaction_markers_are_consistent_pseudonyms) {
    const std::string doc = "a qwertyuiop b qwertyuiop c zxcvbnmasd [NAME_4] d";
    std::vector<pb_redaction_span> spans = {
        {2, 12, "NAME", 0.9f, nullptr}, {15, 25, "NAME", 0.8f, nullptr}, {28, 38, "NAME", 0.7f, nullptr}};
    const Result r = redact(doc, spans, {"NAME"});
    // the same value gets the same marker; numbering starts after the [NAME_4] already in the input
    CHECK(r.output == "a [NAME_5] b [NAME_5] c [NAME_6] [NAME_4] d");
    std::string back;
    CHECK(restore(r.output, r.map, back) && back == doc);
    std::string edited = r.output;
    edited[0] = 'X';
    edited.insert(3, "!");
    CHECK(!restore(edited, r.map, back));
}

TEST(redaction_different_values_never_share_a_marker) {
    // Names in any script keep their letters, and phone numbers all their digits: two different values get two
    // markers; the notations of one value (accents, case, an international prefix) get one (review R1, finding 10).
    auto markers = [](const std::string& doc, const std::vector<std::pair<int64_t, int64_t>>& at, const char* type) {
        std::vector<pb_redaction_span> spans;
        for (const auto& a : at) spans.push_back({a.first, a.second, type, 0.9f, nullptr});
        const Result r = redact(doc, spans, {type});
        pb::json::Value report;
        std::string error, out;
        if (r.ok && pb::json::parse(r.report, report, error))
            for (const pb::json::Value& s : report.get("spans")->array) out += s.get("marker")->string;
        return out;
    };
    const std::string ivan = "\xD0\x98\xD0\xB2\xD0\xB0\xD0\xBD", pyotr = "\xD0\x9F\xD1\x91\xD1\x82\xD1\x80";
    const std::string petr = "\xD0\x9F\xD0\xB5\xD1\x82\xD1\x80";  // the same name, written without the diaeresis
    std::string doc = ivan + " met " + pyotr + " and " + petr;
    CHECK(markers(doc, {{0, 8}, {13, 21}, {26, 34}}, "PERSON_NAME") == "[PERSON_NAME_1][PERSON_NAME_2][PERSON_NAME_2]");
    doc = "\xE6\x9D\x8E\xE9\x9B\xB7 and \xE7\x8E\x8B\xE8\x8A\xB3";  // two Chinese names
    CHECK(markers(doc, {{0, 6}, {11, 17}}, "PERSON_NAME") == "[PERSON_NAME_1][PERSON_NAME_2]");
    doc = "\xCE\x9D\xCE\xAF\xCE\xBA\xCE\xBF\xCF\x82 or \xCE\x9D\xCE\x99\xCE\x9A\xCE\x9F\xCE\xA3";  // one Greek name
    CHECK(markers(doc, {{0, 10}, {14, 24}}, "PERSON_NAME") == "[PERSON_NAME_1][PERSON_NAME_1]");
    doc = "Jos\xC3\xA9 GARCIA and jose  garcia";
    CHECK(markers(doc, {{0, 12}, {17, 29}}, "PERSON_NAME") == "[PERSON_NAME_1][PERSON_NAME_1]");
    doc = "call 212-555-0123 or 312-555-0123 or +1 212 555 0123";  // 10-digit numbers differ in their first digits
    CHECK(markers(doc, {{5, 17}, {21, 33}, {37, 52}}, "PHONE") == "[PHONE_1][PHONE_2][PHONE_1]");
    doc = "+34 612 345 678, 0034612345678, 612 345 678, 612 345 679";
    CHECK(markers(doc, {{0, 15}, {17, 30}, {32, 43}, {45, 56}}, "PHONE") == "[PHONE_1][PHONE_1][PHONE_1][PHONE_2]");
}

TEST(redaction_restore_needs_a_complete_map) {
    // The map written by pb_redact: version 1, the SHA-256 of the output, integral offsets inside the output. Anything
    // else is refused before the output is touched (review R1, finding 18).
    const std::string doc = "hello a@b.c world";
    const Result r = redact(doc, {{6, 11, "EMAIL", 0.9f, nullptr}}, {"EMAIL"});
    std::string back;
    CHECK(restore(r.output, r.map, back) && back == doc);
    auto without = [&](const std::string& field) {  // the map without one of its fields
        pb::json::Value map;
        std::string error;
        pb::json::parse(r.map, map, error);
        map.object.erase(
            std::remove_if(map.object.begin(), map.object.end(),
                           [&](const std::pair<std::string, pb::json::Value>& kv) { return kv.first == field; }),
            map.object.end());
        return pb::json::dump(map);
    };
    CHECK(!restore(r.output + " EDITED", without("sha256_out"), back));
    CHECK(!restore(r.output, without("sha256_out"), back));
    CHECK(!restore(r.output, without("version"), back));
    std::string versioned = r.map;
    versioned.replace(versioned.find("\"version\":1"), 11, "\"version\":99");
    CHECK(!restore(r.output, versioned, back));
    for (const char* offset : {"1e300", "-1", "6.5"}) {
        std::string odd = r.map;
        const size_t at = odd.find("\"out_start\":6");
        CHECK(at != std::string::npos);
        if (at == std::string::npos) continue;
        odd.replace(at, 13, std::string("\"out_start\":") + offset);
        CHECK_MSG(!restore(r.output, odd, back), offset);
    }
}

TEST(redaction_numbers_do_not_depend_on_the_locale) {
    // A host application may set a locale whose decimal separator is ',' (or a character of several bytes): the
    // report's numbers, and the numbers the JSON reader takes, stay those of JSON (review R1, finding 17).
    const std::string doc = "mail me at someone@example.com today";
    const char* comma_locales[] = {"de_DE.UTF-8", "de_DE.utf8", "de-DE", "German_Germany.1252", "fa_IR.UTF-8"};
    int tried = 0;
    for (const char* name : comma_locales) {
        if (!std::setlocale(LC_ALL, name)) continue;
        ++tried;
        const Result r = redact(doc, {{11, 30, "EMAIL", 0.97f, nullptr}}, {"EMAIL"});
        CHECK_MSG(r.report.find("\"confidence\":0.97") != std::string::npos, name + (": " + r.report));
        size_t used = 0;
        CHECK(pb::json::parse_number("0.97e1,", &used) == 9.7 && used == 6);
        CHECK(pb::json::number(0.5) == "0.5" && pb::json::number(-1234.5625) == "-1234.5625");
    }
    std::setlocale(LC_ALL, "C");
    if (!tried) std::printf("     (no locale with another decimal separator is installed: only C was tested)\n");
    size_t used = 0;
    CHECK(pb::json::parse_number("-0.5", &used) == -0.5 && used == 4);
    CHECK(pb::json::parse_number("01", &used) == 0.0 && used == 1);  // RFC 8259: no leading zero
    CHECK(pb::json::parse_number("1.", &used) == 1.0 && used == 1);
    pb::json::parse_number("-.5", &used);
    CHECK(used == 0);
    // dump() writes a number as an integer only when a 64-bit integer holds it (finding 19): 1e19 went through a
    // conversion out of range before.
    pb::json::Value v, back;
    std::string error;
    CHECK_MSG(pb::json::parse("[1e19,-3,4.5e15,-1e300]", v, error), error);
    CHECK_MSG(pb::json::parse(pb::json::dump(v), back, error), error);
    CHECK(back.array.size() == 4 && back.array[0].number == 1e19 && back.array[1].number == -3.0 &&
          back.array[2].number == 4.5e15 && back.array[3].number == -1e300);
}
