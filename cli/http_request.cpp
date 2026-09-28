#include "http_request.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <set>

#include "args.h"
#include "core/digest.h"

namespace cli {

const char* const kDecideTooLarge = "/v1/decide takes at most 4096 bytes of input; use /v1/scan";

namespace {

constexpr size_t kMaxFormParts = 1024;  // cpp-httplib's own limit on the parts of a form

// Which endpoints take a parameter (docs/api.md, "Sending an input"). Throws `bad_parameter` for a parameter of
// another endpoint, or one no endpoint takes: nothing a client asks for is silently ignored.
void check_parameter(const std::string& name, Endpoint endpoint) {
    static const std::set<std::string> kEvery = {"model", "filename", "bias", "min_confidence"};
    static const std::set<std::string> kDetect = {"votes",     "ensemble",   "reveal",       "per_model",
                                                  "prefilter", "early_exit", "strings_only", "query"};
    if (kEvery.count(name)) return;
    if (name == "format") {
        if (endpoint == Endpoint::Scan) return;
        throw HttpError{400, "bad_parameter", "`format` applies to /v1/scan only"};
    }
    if (kDetect.count(name)) {
        if (endpoint != Endpoint::Redact) return;
        throw HttpError{400, "bad_parameter", "`" + name + "` applies to /v1/scan and /v1/decide only"};
    }
    if (name == "types" || name == "map") {
        if (endpoint == Endpoint::Redact) return;
        throw HttpError{400, "bad_parameter", "`" + name + "` applies to /v1/redact only"};
    }
    throw HttpError{400, "bad_parameter", "unknown parameter `" + name + "`"};
}

std::string percent_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                   std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// Parameters of the query string (a form-encoded body is never parsed for them: it is the input itself).
std::multimap<std::string, std::string> query_parameters(const std::string& target) {
    std::multimap<std::string, std::string> out;
    const size_t q = target.find('?');
    if (q == std::string::npos) return out;
    std::string query = target.substr(q + 1);
    query = query.substr(0, query.find('#'));
    for (const std::string& item : split_list(query, '&')) {
        const size_t eq = item.find('=');
        out.emplace(percent_decode(item.substr(0, eq)),
                    eq == std::string::npos ? "" : percent_decode(item.substr(eq + 1)));
    }
    return out;
}

// {"text": "...", "base64": "...", "queries": [...], PARAMETER: string | number | boolean}
void read_json_body(const std::string& body, Request& r) {
    pb::json::Value v;
    std::string error;
    if (!pb::json::parse(body, v, error) || !v.is(pb::json::Value::Type::Object))
        throw HttpError{
            400, "bad_json",
            "the body must be a JSON object like {\"text\": \"...\"}" + (error.empty() ? "" : ": " + error)};
    bool have = false;
    for (const auto& kv : v.object) {
        const pb::json::Value& x = kv.second;
        if (kv.first == "text" && x.is(pb::json::Value::Type::String)) {
            r.input.assign(x.string.begin(), x.string.end());
            have = true;
        } else if (kv.first == "base64" && x.is(pb::json::Value::Type::String)) {
            if (!pb::base64_decode(x.string, r.input)) throw HttpError{400, "bad_json", "`base64` is not valid base64"};
            have = true;
        } else if (kv.first == "queries" && x.is(pb::json::Value::Type::Array)) {
            for (const pb::json::Value& q : x.array) {
                if (!q.is(pb::json::Value::Type::String))
                    throw HttpError{400, "bad_json", "`queries` must be a list of strings"};
                r.params.emplace("query", q.string);
            }
        } else if (x.is(pb::json::Value::Type::String)) {
            r.params.emplace(kv.first, x.string);
        } else if (x.is(pb::json::Value::Type::Number)) {
            r.params.emplace(kv.first, pb::json::number(x.number));
        } else if (x.is(pb::json::Value::Type::Bool)) {
            r.params.emplace(kv.first, x.boolean ? "yes" : "no");
        } else {
            throw HttpError{400, "bad_json", "field `" + kv.first + "` must be a string, number or boolean"};
        }
    }
    if (!have) throw HttpError{400, "no_input", "the JSON body needs `text` or `base64`"};
}

float number_parameter(const Request& r, const std::string& name) {
    char* end = nullptr;
    const std::string v = r.get(name);
    const double x = std::strtod(v.c_str(), &end);
    if (v.empty() || *end || !std::isfinite(x))
        throw HttpError{400, "bad_parameter", "`" + name + "` must be a number"};
    return static_cast<float>(x);
}

}  // namespace

std::string Request::get(const std::string& key, const std::string& fallback) const {
    const auto it = params.find(key);
    return it == params.end() ? fallback : it->second;
}

std::string media_type(const std::string& content_type) {
    std::string type = content_type.substr(0, content_type.find(';'));
    std::transform(type.begin(), type.end(), type.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const size_t first = type.find_first_not_of(" \t"), last = type.find_last_not_of(" \t");
    return first == std::string::npos ? std::string() : type.substr(first, last - first + 1);
}

Request read_request(const httplib::Request& req, const httplib::ContentReader& content, Endpoint endpoint,
                     uint64_t& received) {
    Request r;
    r.params = query_parameters(req.target);
    // /v1/decide stops reading past its limit, chunked bodies included; the others read up to --max-body-mb, which
    // cpp-httplib enforces (413 payload_too_large).
    const uint64_t cap = endpoint == Endpoint::Decide ? kDecideBodyLimit : UINT64_MAX;
    bool over_cap = false, too_many_parts = false;
    const auto count = [&](size_t n) {
        received += n;
        over_cap = over_cap || received > cap;
        return !over_cap;
    };
    struct Part {
        std::string name, filename, content;
    };
    std::vector<Part> parts;
    const bool multipart = req.is_multipart_form_data();  // the test cpp-httplib's reader makes
    const auto part_header = [&](const httplib::FormData& part) {
        too_many_parts = parts.size() >= kMaxFormParts;
        if (!too_many_parts) parts.push_back({part.name, part.filename, std::string()});
        return !too_many_parts;
    };
    const auto part_data = [&](const char* data, size_t n) {
        if (!count(n) || parts.empty()) return false;
        parts.back().content.append(data, n);
        return true;
    };
    const auto body_data = [&](const char* data, size_t n) {
        if (!count(n)) return false;
        r.input.insert(r.input.end(), data, data + n);
        return true;
    };
    const bool read = multipart ? content(part_header, part_data) : content(body_data);
    if (over_cap) throw HttpError{413, "too_large_for_decide", kDecideTooLarge};
    if (too_many_parts) throw HttpError{400, "bad_request", "the form has more than 1024 parts"};
    if (!read) throw HttpError{400, "bad_request", "the request body could not be read"};
    if (multipart) {
        bool have_file = false, have_text = false;
        for (Part& part : parts) {
            if (!part.filename.empty()) {  // a file part: the input
                if (part.name != "file")
                    throw HttpError{400, "bad_parameter",
                                    "the form's file part must be named `file`, not `" + part.name + "`"};
                if (have_file) throw HttpError{400, "bad_request", "the form has more than one `file` part"};
                have_file = true;
                r.input.assign(part.content.begin(), part.content.end());
                r.filename = part.filename;
            } else if (part.name == "text") {  // or a text field
                if (have_text) throw HttpError{400, "bad_request", "the form has more than one `text` field"};
                have_text = true;
                if (!have_file) r.input.assign(part.content.begin(), part.content.end());
            } else {
                r.params.emplace(part.name, std::move(part.content));
            }
        }
        if (have_file && have_text)
            throw HttpError{400, "bad_request", "give the form a `file` part or a `text` field, not both"};
        if (!have_file && !have_text)
            throw HttpError{400, "no_input", "the form needs a `file` part or a `text` field"};
    } else if (media_type(req.get_header_value("Content-Type")) == "application/json") {
        const std::string body(r.input.begin(), r.input.end());
        std::vector<uint8_t>().swap(r.input);  // the body is the JSON document, not the input: free it
        read_json_body(body, r);
    }
    if (r.filename.empty()) r.filename = r.get("filename");
    for (const auto& kv : r.params) check_parameter(kv.first, endpoint);
    return r;
}

bool truthy(const std::string& value, const std::string& name) {
    if (value == "yes" || value == "on" || value == "true" || value == "1") return true;
    if (value == "no" || value == "off" || value == "false" || value == "0") return false;
    throw HttpError{400, "bad_parameter", "`" + name + "` takes yes or no, not `" + value + "`"};
}

DetectSettings settings_of(const Request& r, const Setup& s) {
    DetectSettings d = s.settings;
    if (r.has("bias")) {
        d.has_bias = true;
        d.bias = number_parameter(r, "bias");
    }
    if (r.has("min_confidence")) d.min_confidence = number_parameter(r, "min_confidence");
    const int members = static_cast<int>(s.specialist.members.size());
    if (r.has("votes")) {
        const float v = number_parameter(r, "votes");
        if (v < 1 || v > members || v != static_cast<int>(v))
            throw HttpError{400, "bad_parameter", "`votes` must be an integer from 1 to " + std::to_string(members)};
        d.votes = static_cast<int>(v);
    }
    if (r.has("ensemble")) d.no_ensemble = !truthy(r.get("ensemble"), "ensemble");
    if (r.has("reveal")) d.reveal = truthy(r.get("reveal"), "reveal");
    if (r.has("per_model")) d.per_model = truthy(r.get("per_model"), "per_model");
    if (r.has("prefilter")) d.prefilter = truthy(r.get("prefilter"), "prefilter");
    if (r.has("strings_only")) d.strings_only = truthy(r.get("strings_only"), "strings_only");
    if (r.has("early_exit")) d.early_exit = truthy(r.get("early_exit"), "early_exit");
    for (auto it = r.params.equal_range("query"); it.first != it.second; ++it.first)
        d.queries.push_back(it.first->second);
    return d;
}

}  // namespace cli
