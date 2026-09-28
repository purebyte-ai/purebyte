#include "sarif.h"

#include <algorithm>
#include <map>

#include "core/digest.h"
#include "detection.h"
#include "purebyte/pb.h"

namespace cli {

std::string artifact_uri(const std::string& path) {
    std::string p = path;
    std::replace(p.begin(), p.end(), '\\', '/');
    while (p.compare(0, 2, "./") == 0) p.erase(0, 2);  // "./src/a.py": code scanning wants "src/a.py"
    std::string out = p.size() > 1 && p[1] == ':' ? "file:///" : !p.empty() && p[0] == '/' ? "file://" : "";
    static const char* kHex = "0123456789ABCDEF";
    for (const unsigned char c : p) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (plain || c == '-' || c == '.' || c == '_' || c == '~' || c == '/' || c == ':' || c == '!') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

std::string render_sarif(const std::vector<SarifFinding>& findings, const std::vector<Failure>& failures,
                         const std::string& model_name, const std::string& model_version, bool reveal) {
    std::vector<std::string> kinds;  // one rule per kind, in order of first appearance
    for (const SarifFinding& f : findings) {
        const std::string kind = string_of(*f.finding, "kind");
        if (std::find(kinds.begin(), kinds.end(), kind) == kinds.end()) kinds.push_back(kind);
    }
    pb::json::Writer w;
    w.begin_object().field("$schema", "https://json.schemastore.org/sarif-2.1.0.json").field("version", "2.1.0");
    w.key("runs").begin_array().begin_object();
    w.key("tool")
        .begin_object()
        .key("driver")
        .begin_object()
        .field("name", "purebyte")
        .field("semanticVersion", pb_version());
    w.key("rules").begin_array();
    for (const std::string& kind : kinds)
        w.begin_object()
            .field("id", "purebyte/" + kind)
            .field("name", kind)
            .key("shortDescription")
            .begin_object()
            .field("text", "Possible " + kind)
            .end_object()
            .key("fullDescription")
            .begin_object()
            .field("text", "A PureByte byte-level model marked these bytes as `" + kind +
                               "`. Review what it flags before acting.")
            .end_object()
            .key("defaultConfiguration")
            .begin_object()
            .field("level", "error")
            .end_object()
            .key("properties")
            .begin_object()
            .field("model", model_name)
            .field("modelVersion", model_version)
            .end_object()
            .end_object();
    w.end_array().end_object().end_object();

    w.key("invocations").begin_array().begin_object().field("executionSuccessful", failures.empty());
    w.key("toolExecutionNotifications").begin_array();
    for (const Failure& f : failures) {
        w.begin_object().field("level", "error").key("message").begin_object().field("text", f.reason).end_object();
        w.key("locations").begin_array().begin_object().key("physicalLocation").begin_object();
        w.key("artifactLocation").begin_object().field("uri", artifact_uri(f.file)).end_object();
        w.end_object().end_object().end_array().end_object();
    }
    w.end_array().end_object().end_array();

    w.field("columnKind", "unicodeCodePoints").key("results").begin_array();
    std::map<std::string, int> occurrences;  // per (file, masked value): keeps fingerprints stable and distinct
    for (const SarifFinding& f : findings) {
        const pb::json::Value& v = *f.finding;
        const std::string kind = string_of(v, "kind"), masked = string_of(v, "snippet_masked");
        const std::string file = string_of(v, "file");
        const std::string uri = artifact_uri(file.empty() ? std::string("input") : file);
        const std::string key = uri + "\n" + masked;
        const std::string fingerprint =
            pb::sha256_hex(key.data(), key.size()).substr(0, 24) + "/" + std::to_string(occurrences[key]++);
        const long long start = number_of(v, "start"), end = number_of(v, "end");
        const bool warning = is_warning(v);
        w.begin_object()
            .field("ruleId", "purebyte/" + kind)
            .field("ruleIndex", static_cast<int64_t>(std::find(kinds.begin(), kinds.end(), kind) - kinds.begin()))
            .field("level", warning ? "warning" : "error")
            .key("message")
            .begin_object()
            .field("text",
                   "possible " + kind + ": " + masked + (warning ? " (in a test, example or documentation path)" : ""))
            .end_object();
        w.key("locations").begin_array().begin_object().key("physicalLocation").begin_object();
        w.key("artifactLocation").begin_object().field("uri", uri).end_object();
        w.key("region").begin_object();
        if (const long long line = number_of(v, "line")) {
            w.field("startLine", static_cast<int64_t>(line));
            if (f.column > 0) w.field("startColumn", static_cast<int64_t>(f.column));
            if (v.get("end_line")) w.field("endLine", static_cast<int64_t>(number_of(v, "end_line")));
        }
        w.field("byteOffset", static_cast<int64_t>(start)).field("byteLength", static_cast<int64_t>(end - start));
        const std::string snippet = reveal && v.get("snippet") ? string_of(v, "snippet") : masked;
        w.key("snippet").begin_object().field("text", snippet).end_object();
        w.end_object().end_object().end_object().end_array();
        w.key("partialFingerprints").begin_object().field("purebyteFinding/v1", fingerprint).end_object();
        w.key("properties").begin_object().field("confidence", v.get("confidence") ? v.get("confidence")->number : 0.0);
        w.field("votes", static_cast<int64_t>(number_of(v, "votes", 1)));
        if (v.get("inside_base64")) w.field("insideBase64", true);
        w.end_object().end_object();
    }
    w.end_array().end_object().end_array().end_object();
    return w.take();
}

}  // namespace cli
