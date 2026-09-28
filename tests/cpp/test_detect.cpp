// pb_detect on the random models of tests/gen: every option combination returns a well-formed result (no crash on
// windows that the exit head stopped or the prefilter skipped), spans stay inside the input, no detected value
// appears in clear unless it was asked for, and findings carry the severity of their path inside the project. And,
// on crafted models whose spans are known (crafted_models.h): every view hides every value any model found, NaN head
// values, short inputs and UTF-16 inputs of pb_redact, and the cost of documents with very many findings.
#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include "core/json.h"
#include "crafted_models.h"
#include "purebyte/pb.h"
#include "test.h"

namespace {

struct Detector {
    pb_model* model = nullptr;
    pb_session* session = nullptr;
    pb_detector* detector = nullptr;
    ~Detector() {
        pb_detector_free(detector);
        pb_session_free(session);
        pb_model_free(model);
    }
};

bool open(Detector& d, const char* name, const char* profile) {
    const std::string path = pbtest::model_path(name);
    if (path.empty()) return false;
    pb_error err;
    pb_session_options so;
    pb_session_options_init(&so);
    so.threads = 3;
    CHECK(pb_model_load(path.c_str(), &d.model, &err) == PB_OK);
    CHECK(pb_session_create(&so, &d.session, &err) == PB_OK);
    CHECK(d.model && pb_detector_create(&d.model, 1, profile, &d.detector, &err) == PB_OK);
    return d.detector != nullptr;
}

std::vector<uint8_t> text_input(size_t n) {
    pbtest::Random rng(5);
    std::vector<uint8_t> out(n);
    for (uint8_t& b : out) b = static_cast<uint8_t>(rng.below(8) ? 'a' + rng.below(26) : (rng.below(2) ? ' ' : '\n'));
    return out;
}

bool detect(Detector& d, const std::vector<uint8_t>& bytes, uint32_t flags, pb::json::Value& out) {
    const pb_input input{bytes.data(), bytes.size()};
    const char* names[] = {"input.txt"};
    pb_detect_options o;
    pb_detect_options_init(&o);
    o.flags = flags;
    char* json = nullptr;
    pb_error err;
    const pb_status status = pb_detect(d.session, d.detector, &input, 1, names, &o, &json, &err);
    CHECK_MSG(status == PB_OK, err.message);
    if (status != PB_OK) return false;
    std::string error;
    const bool ok = pb::json::parse(json, out, error);
    CHECK_MSG(ok, error);
    pb_free(json);
    return ok;
}

}  // namespace

TEST(detect_survives_exited_and_skipped_windows) {
    Detector d;
    if (!open(d, "ssm-causal-ternary", "none")) return;
    const std::vector<uint8_t> bytes = text_input(3000);
    for (uint32_t flags : {0u, unsigned(PB_DETECT_EARLY_EXIT), unsigned(PB_DETECT_PREFILTER),
                           unsigned(PB_DETECT_EARLY_EXIT | PB_DETECT_PREFILTER)}) {
        pb::json::Value r;
        if (!detect(d, bytes, flags, r)) continue;
        const pb::json::Value* results = r.get("results");
        CHECK(results && results->array.size() == 1);
        if (!results || results->array.empty()) continue;
        const pb::json::Value& one = results->array[0];
        CHECK(one.get("bytes") && one.get("bytes")->number == 3000);
        if (flags & PB_DETECT_EARLY_EXIT) CHECK(one.get("windows_exited") != nullptr);
        for (const pb::json::Value& s : one.get("spans")->array)
            CHECK(s.get("start")->number >= 0 && s.get("end")->number <= 3000 &&
                  s.get("start")->number < s.get("end")->number);
    }
}

TEST(detect_masks_values_unless_revealed) {
    Detector d;
    if (!open(d, "attention-lookahead", "none")) return;  // its tag head is not gated: it finds spans in any text
    const std::vector<uint8_t> bytes = text_input(2000);
    pb::json::Value masked, revealed;
    if (!detect(d, bytes, 0, masked)) return;
    if (!detect(d, bytes, PB_DETECT_REVEAL, revealed)) return;
    const pb::json::Value* findings = masked.get("findings");
    CHECK(findings != nullptr && !findings->array.empty());
    if (!findings) return;
    for (const pb::json::Value& f : findings->array) {
        CHECK(f.get("snippet_masked") != nullptr);
        CHECK(f.get("snippet") == nullptr && f.get("context") == nullptr);
    }
    for (const pb::json::Value& f : revealed.get("findings")->array) CHECK(f.get("snippet") != nullptr);
}

namespace {

// Every finding's severity, by the `input` of the result it belongs to (a result's findings are its spans, in order).
std::vector<std::vector<std::string>> severities_by_input(const pb::json::Value& r, size_t inputs) {
    std::vector<std::vector<std::string>> out(inputs);
    size_t next = 0;
    for (const pb::json::Value& one : r.get("results")->array)
        for (size_t i = 0; i < one.get("spans")->array.size(); ++i, ++next) {
            const pb::json::Value* severity = r.get("findings")->array.at(next).get("severity");
            out.at(static_cast<size_t>(one.get("input")->number)).push_back(severity ? severity->string : "(none)");
        }
    return out;
}

bool all_are(const std::vector<std::string>& values, const char* expected) {
    for (const std::string& v : values)
        if (v != expected) return false;
    return !values.empty();
}

}  // namespace

TEST(detect_severity_follows_the_path_inside_the_project) {
    Detector d;
    if (!open(d, "attention-lookahead", "secrets-code")) return;
    CHECK(pb_detector_path_severity(d.detector, "tests/fixtures/app.cfg") == PB_SEVERITY_WARNING);
    CHECK(pb_detector_path_severity(d.detector, "src\\test\\Keys.java") == PB_SEVERITY_WARNING);
    CHECK(pb_detector_path_severity(d.detector, "src/app.cfg") == PB_SEVERITY_ERROR);
    CHECK(pb_detector_path_severity(d.detector, nullptr) == PB_SEVERITY_ERROR);
    CHECK(pb_detector_path_severity(nullptr, "tests/a.py") == PB_SEVERITY_ERROR);

    std::string text = "# settings\nhost = example.internal\nport = 8080\n";  // python/tests/support.py's TEXT
    for (int i = 0; i < 12; ++i) text += "user = admin\nmode = fast\n";
    text += "note = the quick brown fox jumps over the lazy dog\n";
    const std::vector<uint8_t> bytes(text.begin(), text.end());
    // The same bytes under four names. The second one's folders above the project hold a `test`: its path inside the
    // project (options.paths) is what counts; the others have no path, so their names count.
    const pb_input inputs[] = {{bytes.data(), bytes.size()},
                               {bytes.data(), bytes.size()},
                               {bytes.data(), bytes.size()},
                               {bytes.data(), bytes.size()}};
    const char* names[] = {"src/app.cfg", "/home/me/test/repo/src/app.cfg", "tests/app.cfg", "docs/guide.md"};
    const char* paths[] = {nullptr, "src/app.cfg", nullptr, nullptr};
    pb_detect_options o;
    pb_detect_options_init(&o);
    o.use_bias = 1;
    o.bias = 30.f;  // marks nearly every byte
    o.paths = paths;
    char* json = nullptr;
    pb_error err;
    CHECK_MSG(pb_detect(d.session, d.detector, inputs, 4, names, &o, &json, &err) == PB_OK, err.message);
    if (!json) return;
    pb::json::Value r;
    std::string error;
    CHECK_MSG(pb::json::parse(json, r, error), error);
    pb_free(json);
    const auto by_input = severities_by_input(r, 4);
    CHECK(all_are(by_input[0], "error"));
    CHECK(all_are(by_input[1], "error"));
    CHECK(all_are(by_input[2], "warning"));
    CHECK(all_are(by_input[3], "warning"));

    // A caller built before `paths` was appended passes the smaller struct: accepted, and the names count.
    o.struct_size = static_cast<uint32_t>(offsetof(pb_detect_options, paths));
    CHECK_MSG(pb_detect(d.session, d.detector, inputs, 4, names, &o, &json, &err) == PB_OK, err.message);
    if (!json) return;
    pb::json::Value old;
    CHECK_MSG(pb::json::parse(json, old, error), error);
    pb_free(json);
    CHECK(all_are(severities_by_input(old, 4)[1], "warning"));
}

TEST(detect_severity_is_error_without_path_rules) {
    Detector d;
    if (!open(d, "attention-lookahead", "none")) return;
    CHECK(pb_detector_path_severity(d.detector, "tests/fixtures/app.cfg") == PB_SEVERITY_ERROR);
    const std::vector<uint8_t> bytes = text_input(2000);
    const pb_input input{bytes.data(), bytes.size()};
    const char* names[] = {"tests/docs/example.md"};
    pb_detect_options o;
    pb_detect_options_init(&o);
    char* json = nullptr;
    pb_error err;
    CHECK_MSG(pb_detect(d.session, d.detector, &input, 1, names, &o, &json, &err) == PB_OK, err.message);
    if (!json) return;
    pb::json::Value r;
    std::string error;
    CHECK_MSG(pb::json::parse(json, r, error), error);
    pb_free(json);
    CHECK(all_are(severities_by_input(r, 1)[0], "error"));
}

TEST(detect_allow_marker_accepts_the_findings_of_its_line) {
    // secrets-code: a finding that starts on a line carrying `purebyte:allow` is not reported. With a very high bias
    // the random model marks nearly every byte, so without the marker every line has findings.
    Detector d;
    if (!open(d, "attention-lookahead", "secrets-code")) return;
    auto text_with = [](const char* comment) {
        std::string text;
        for (int i = 0; i < 16; ++i)
            text += "token_" + std::to_string(i) + " = value" + std::to_string(i) + comment + "\n";
        return std::vector<uint8_t>(text.begin(), text.end());
    };
    auto count = [&](const std::vector<uint8_t>& bytes) -> size_t {
        const pb_input input{bytes.data(), bytes.size()};
        const char* names[] = {"src/app.cfg"};
        pb_detect_options o;
        pb_detect_options_init(&o);
        o.use_bias = 1;
        o.bias = 30.f;
        char* json = nullptr;
        pb_error err;
        if (pb_detect(d.session, d.detector, &input, 1, names, &o, &json, &err) != PB_OK) return 0;
        pb::json::Value v;
        std::string error;
        const bool ok = pb::json::parse(json, v, error);
        pb_free(json);
        const pb::json::Value* findings = ok ? v.get("findings") : nullptr;
        return findings ? findings->array.size() : 0;
    };
    CHECK(count(text_with("  # purebyte:allOW")) > 0);  // not the marker: findings as usual
    CHECK(count(text_with("  # purebyte:allow")) == 0);
}

namespace {

// A detector over crafted models (their files), with a profile.
struct Crafted {
    std::vector<pb_model*> models;
    pb_session* session = nullptr;
    pb_detector* detector = nullptr;
    ~Crafted() {
        pb_detector_free(detector);
        pb_session_free(session);
        for (pb_model* m : models) pb_model_free(m);
    }
};

bool crafted(Crafted& c, const std::vector<std::vector<uint8_t>>& files, const char* profile, int threads = 2) {
    for (const std::vector<uint8_t>& file : files) {
        pb_model* m = nullptr;
        std::string message;
        CHECK_MSG(pbtest::load_model(file, &m, &message) == PB_OK, message);
        if (!m) return false;
        c.models.push_back(m);
    }
    pb_session_options so;
    pb_session_options_init(&so);
    so.threads = threads;
    pb_error err;
    CHECK_MSG(pb_session_create(&so, &c.session, &err) == PB_OK, err.message);
    CHECK_MSG(pb_detector_create(c.models.data(), c.models.size(), profile, &c.detector, &err) == PB_OK, err.message);
    return c.session && c.detector;
}

bool run_detect(const Crafted& c, const std::string& text, uint32_t flags, int votes, pb::json::Value& out) {
    const pb_input input{reinterpret_cast<const uint8_t*>(text.data()), text.size()};
    const char* names[] = {"input.bin"};
    pb_detect_options o;
    pb_detect_options_init(&o);
    o.flags = flags;
    o.votes = votes;
    char* json = nullptr;
    pb_error err;
    const pb_status status = pb_detect(c.session, c.detector, &input, 1, names, &o, &json, &err);
    CHECK_MSG(status == PB_OK, err.message);
    if (status != PB_OK) return false;
    std::string error;
    const bool ok = pb::json::parse(json, out, error);
    CHECK_MSG(ok, error);
    pb_free(json);
    return ok;
}

// Every masked view of every finding: the value, its line or string; top level and per ensemble member.
std::vector<std::string> masked_views(const pb::json::Value& r) {
    std::vector<std::string> out;
    auto collect = [&](const pb::json::Value& f) {
        for (const char* key : {"snippet_masked", "context_masked", "string_masked"})
            if (const pb::json::Value* v = f.get(key)) out.push_back(v->string);
    };
    for (const pb::json::Value& f : r.get("findings")->array) collect(f);
    for (const pb::json::Value& one : r.get("results")->array)
        if (const pb::json::Value* per = one.get("per_model"))
            for (const pb::json::Value& member : per->array)
                for (const pb::json::Value& f : member.array) collect(f);
    return out;
}

std::string redacted_output(pb_redaction* r) {
    size_t n = 0;
    const uint8_t* out = pb_redaction_output(r, &n);
    return std::string(reinterpret_cast<const char*>(out), n);
}

}  // namespace

TEST(detect_binary_strings_hide_every_value_the_model_found) {
    // secrets-binary reports the first of two values of one printable string and folds the second into it ("the same
    // finding"): the string shown must hide both (review R1, finding 1).
    Crafted c;
    if (!crafted(c, {pbtest::flag_model(pbtest::is_upper)}, "secrets-binary")) return;
    const std::string first = std::string("AKIA") + "QWERTYUIOP", second = std::string("ZXCV") + "BNMASDFGHJKL";
    const std::string text =
        std::string("\0\1", 2) + "id=" + first + " secret=" + second + " end" + std::string(1, '\0');
    for (uint32_t flags : {0u, unsigned(PB_DETECT_STRINGS_ONLY)}) {
        pb::json::Value r;
        if (!run_detect(c, text, flags, 0, r)) continue;
        CHECK(r.get("findings")->array.size() == 1);
        for (const std::string& view : masked_views(r))
            CHECK_MSG(pbtest::hidden_in(first, view) && pbtest::hidden_in(second, view), view);
    }
}

TEST(detect_ensemble_views_hide_every_member_finding) {
    // Two members, one marking capitals and one digits: whatever the votes needed and with or without every member's
    // own results, no view shows a value that some member found (review R1, finding 3).
    Crafted c;
    if (!crafted(c, {pbtest::flag_model(pbtest::is_upper), pbtest::flag_model(pbtest::is_digit)}, "none")) return;
    const std::string letters = std::string("ABCDE") + "FGHIJK", digits = std::string("4711") + "0235";
    const std::string text = "key=" + letters + " pin=" + digits + " end\n";
    size_t views = 0;
    for (int votes : {1, 2})
        for (uint32_t flags : {0u, unsigned(PB_DETECT_PER_MODEL)}) {
            pb::json::Value r;
            if (!run_detect(c, text, flags, votes, r)) continue;
            for (const std::string& view : masked_views(r)) {
                ++views;
                CHECK_MSG(pbtest::hidden_in(letters, view) && pbtest::hidden_in(digits, view), view);
            }
            // Both votes needed: each value has one, so none is reported (member results only, when asked).
            if (votes == 2) CHECK(r.get("findings")->array.empty());
        }
    CHECK(views > 0);
}

TEST(detect_non_finite_head_values_are_not_positive) {
    // A choice head whose logits overflow to infinity has NaN probabilities in every window: no window is the most
    // positive, none counts as positive, and the result is written (review R1, finding 2; it crashed).
    pbtest::FlagOptions options;
    options.choice_head = true;
    options.choice_gain = 3.0e38f;
    Crafted c;
    if (!crafted(c, {pbtest::flag_model(pbtest::is_upper, options)}, "none")) return;
    pb::json::Value r;
    if (!run_detect(c, "hello WORLD, a line long enough for two windows of this model, and a few more bytes\n", 0, 0,
                    r))
        return;
    const pb::json::Value& one = r.get("results")->array.at(0);
    CHECK(one.get("windows")->number == 2);
    CHECK(one.get("positive_windows")->number == 0);
    const pb::json::Value* choice = one.get("decisions")->get("choice");
    CHECK(choice != nullptr && choice->get("index")->number == 0 &&
          choice->get("probability")->is(pb::json::Value::Type::Null));
    CHECK(r.get("findings")->array.size() == 5);  // the tag head does not depend on it: W, O, R, L, D
}

TEST(detect_redact_analyzes_short_inputs_with_every_profile) {
    // An input shorter than the model's shortest window (24 bytes here) is analyzed and redacted whatever the profile
    // (review R1, finding 9: the secrets profiles returned it in clear, with a report that said it was checked).
    pbtest::FlagOptions options;
    options.min = 24;
    const std::string value = std::string("QWERT") + "YUIOP";
    const std::string text = "pw=" + value + "\n";
    for (const char* profile : {"none", "redact", "secrets-code", "secrets-binary"}) {
        Crafted c;
        if (!crafted(c, {pbtest::flag_model(pbtest::is_upper, options)}, profile)) return;
        pb_redaction* red = nullptr;
        pb_error err;
        const pb_status status = pb_redact(c.session, c.detector, reinterpret_cast<const uint8_t*>(text.data()),
                                           text.size(), nullptr, &red, &err);
        CHECK_MSG(status == PB_OK, std::string(profile) + ": " + err.message);
        if (!red) continue;
        const std::string output = redacted_output(red);
        CHECK_MSG(pbtest::hidden_in(value, output) && output.find("[entity_0_1]") != std::string::npos, profile);
        CHECK_MSG(std::string(pb_redaction_report(red)).find("\"marker\":\"[entity_0_1]\"") != std::string::npos,
                  profile);
        pb_redaction_free(red);
    }
}

TEST(detect_redact_refuses_inputs_over_the_profile_limit) {
    Crafted c;
    if (!crafted(c, {pbtest::flag_model(pbtest::is_upper)}, "secrets-code")) return;
    const std::vector<uint8_t> big(static_cast<size_t>(pb_detector_max_input_bytes(c.detector)) + 1, 'a');
    pb_redaction* red = nullptr;
    pb_error err;
    CHECK(pb_redact(c.session, c.detector, big.data(), big.size(), nullptr, &red, &err) == PB_ERR_ARGUMENT);
    CHECK(red == nullptr && std::string(err.message).find("over this model's limit") != std::string::npos);
}

TEST(detect_redact_keeps_the_encoding_of_utf16_inputs) {
    // secrets-code reads a UTF-16 file as UTF-8. Its redaction is a copy of the UTF-16 bytes all the same (review R1,
    // finding 24): markers written in UTF-16, every other byte as it was, and the map restores the input.
    Crafted c;
    if (!crafted(c, {pbtest::flag_model(pbtest::is_upper)}, "secrets-code")) return;
    const std::string value = std::string("QWERTY") + "UIOPASDF";
    const std::string ascii = "token = " + value + ", a line long enough\nnext line \xc3\xa9t\xc3\xa9\n";
    for (bool big_endian : {false, true}) {
        // The UTF-16 encoding of `ascii` (its only non-ASCII character, e acute, is U+00E9).
        std::string in = big_endian ? std::string("\xFE\xFF", 2) : std::string("\xFF\xFE", 2);
        for (size_t i = 0; i < ascii.size(); ++i) {
            uint32_t unit = static_cast<unsigned char>(ascii[i]);
            if (unit == 0xC3) unit = 0xC0 | (static_cast<unsigned char>(ascii[++i]) & 0x3F);
            in += static_cast<char>(big_endian ? unit >> 8 : unit & 0xFF);
            in += static_cast<char>(big_endian ? unit & 0xFF : unit >> 8);
        }
        pb_redact_options ro;
        pb_redact_options_init(&ro);
        ro.flags = PB_REDACT_WITH_MAP;
        ro.name = "notes.txt";
        pb_redaction* red = nullptr;
        pb_error err;
        CHECK_MSG(pb_redact(c.session, c.detector, reinterpret_cast<const uint8_t*>(in.data()), in.size(), &ro, &red,
                            &err) == PB_OK,
                  err.message);
        if (!red) continue;
        const std::string output = redacted_output(red);
        CHECK(output.size() % 2 == 0 && output.compare(0, 2, in, 0, 2) == 0);
        std::string text;  // the output, one character per UTF-16 unit
        bool utf16 = true;
        for (size_t i = 2; i + 1 < output.size(); i += 2) {
            const char low = big_endian ? output[i + 1] : output[i], high = big_endian ? output[i] : output[i + 1];
            text += low;
            utf16 = utf16 && (high == 0 || (low == '\xe9' && high == 0));
        }
        CHECK(utf16);
        CHECK_MSG(pbtest::hidden_in(value, text) && text.rfind("token = [entity_0_1]", 0) == 0 &&
                      text.find("\nnext line \xe9t\xe9\n") != std::string::npos,
                  text);
        const std::string report = pb_redaction_report(red);
        CHECK_MSG(
            report.find(big_endian ? "\"encoding\":\"utf-16be\"" : "\"encoding\":\"utf-16le\"") != std::string::npos &&
                report.find("\"input\":\"notes.txt\"") != std::string::npos &&
                report.find("\"bytes_in\":" + std::to_string(in.size())) != std::string::npos,
            report);
        uint8_t* back = nullptr;
        size_t size = 0;
        CHECK_MSG(pb_restore(reinterpret_cast<const uint8_t*>(output.data()), output.size(), pb_redaction_map(red),
                             &back, &size, &err) == PB_OK,
                  err.message);
        CHECK(back && std::string(reinterpret_cast<const char*>(back), size) == in);
        pb_free(back);
        pb_redaction_free(red);
    }
}

TEST(detect_many_findings_through_the_api) {
    // pb_detect end to end on 20,000 findings on one line, for an ensemble of three members and every profile (the
    // cost of 10^5 findings, without the JSON, is measured by masking_many_findings_cost_little).
    std::string one_line;
    for (int i = 0; i < 20000; ++i) one_line += "x A y ";
    const std::vector<uint8_t> file = pbtest::flag_model(pbtest::is_upper);
    for (const char* profile : {"none", "secrets-code", "secrets-binary"}) {
        Crafted c;
        if (!crafted(c, {file, file, file}, profile, 4)) return;
        const auto start = std::chrono::steady_clock::now();
        pb::json::Value r;
        const bool ok = run_detect(c, one_line, 0, 0, r);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        CHECK_MSG(seconds < 60.0, std::string(profile) + ": " + std::to_string(seconds) + " s");
        if (!ok) continue;
        const size_t findings = r.get("findings")->array.size();
        // secrets-binary reports a string equal to the previous finding's less than 512 bytes after it as the same
        // finding: one every 86 spans of 6 bytes.
        const size_t expected = std::string(profile) == "secrets-binary" ? 233 : 20000;
        CHECK_MSG(findings == expected, std::string(profile) + ": " + std::to_string(findings) + " findings");
        for (const pb::json::Value& f : r.get("findings")->array) CHECK(f.get("votes")->number == 3);
    }
}
