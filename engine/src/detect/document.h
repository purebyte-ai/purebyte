// What the detection profiles consume and produce: documents in, one result per document out.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace pb::detect {

struct Document {
    std::string name;            // display name; empty for an anonymous input
    std::vector<uint8_t> bytes;  // what is scanned (a profile may convert it, e.g. UTF-16 to UTF-8)
};

// Something that could not be analyzed, and why (e.g. an encrypted archive member, a file over the size limit).
struct ScanFailure {
    std::string file, reason;
};

// How much a finding matters (spec/OUTPUT.md, section 5). Findings are errors unless the profile lowers the findings
// of a document to warnings by its path (Profile::path_severity).
enum class Severity { error, warning };

inline const char* severity_name(Severity s) { return s == Severity::warning ? "warning" : "error"; }

// A span after the profile's post-processing. Offsets refer to Document::bytes.
struct Finding {
    int64_t start = 0;
    int64_t end = 0;   // exclusive
    int32_t type = 0;  // entity index of the tag head
    float confidence = 0.f;
    int votes = 1;  // ensemble members that agree
    int model = 0;  // the member that produced it (0 = the main model)
    // Text documents: 1-based line and byte column of `start`, line of the last byte.
    int64_t line = 0, col = 0, end_line = 0;
    std::string text;  // the detected bytes as text: printed only when the caller asks to reveal them
    size_t text_code_points = 0;
    bool multiline = false;  // the detected text spans more than one line
    std::string context;     // the line around it, bounded on a long line (revealed output only; detect/text.h)
    // A finding inside a decoded base64 run: [start, end) is the run, [inner_start, inner_end) the decoded bytes.
    bool inside_base64 = false;
    int64_t inner_start = 0, inner_end = 0;
    // Binary documents: the printable string around the span, [string_start, string_end) of the document (its text is
    // read from the document when the caller reveals it: a long string is not copied for each finding).
    bool binary = false;
    int64_t string_start = 0, string_end = 0;
};

// A stretch of a document that a model marked and that must not appear in clear in any view of the findings (the
// context lines and strings), whether or not a finding reports it: a span a profile rule dropped, a span in the same
// string as an earlier finding, the finding of an ensemble member that did not get the votes. `base64`: the stretch
// is a base64 run whose decoded text a model marked (it is shown as "[base64 blob, N B]").
struct HiddenSpan {
    int64_t start = 0, end = 0;
    bool base64 = false;
};

// A window-level head, aggregated over the document's windows.
struct HeadSummary {
    int head = 0;
    std::string head_name;
    std::string type;  // choice, multilabel, score, ordinal
    std::string aggregate;
    bool computed = false;           // false when no window computed it
    int32_t label = -1;              // choice: class; ordinal: level
    std::vector<float> values;       // probabilities / scores / cumulative probabilities
    std::vector<bool> on;            // multilabel: labels at or above their threshold
    std::vector<std::string> names;  // class, label, output or level names (may be empty)
};

// One label per byte run, for byte_map heads.
struct ByteRegion {
    int64_t start = 0, end = 0;
    int32_t label = 0;
};

struct ByteMapSummary {
    int head = 0;
    std::string head_name;
    std::vector<std::string> names;
    std::vector<ByteRegion> regions;
};

// Query-conditioned models: a value found for one of the asked fields.
struct Field {
    std::string name;  // the query it answers ("" when it could not be attributed)
    int64_t start = 0, end = 0;
    float confidence = 0.f;
    std::string value;  // revealed output only
};

struct DocumentResult {
    std::string name;
    int64_t bytes = 0;
    int64_t windows = 0, windows_skipped = 0, windows_exited = 0, positive_windows = 0;
    float max_positive = 0.f;  // highest 1 - P(class 0) of the first choice head over the windows
    std::vector<HeadSummary> heads;
    std::vector<ByteMapSummary> byte_maps;
    std::vector<Finding> findings;                // the profile's spans, after the ensemble vote
    std::vector<std::vector<Finding>> per_model;  // each member's own findings, when asked for
    // Every span of every window of every ensemble member, and every member's findings, sorted and merged: the masked
    // views hide them too (spec/OUTPUT.md, section 6). Never listed.
    std::vector<HiddenSpan> hidden;
    std::vector<Field> fields;
    std::vector<std::string> type_names;      // entity names of the tag head
    std::map<std::string, int64_t> counters;  // profile-specific counts (e.g. ignored documentation examples)
    bool binary = false;                      // offsets and strings, not lines and columns
    std::string note;                         // why nothing was evaluated, when that is the case
    Severity severity = Severity::error;      // of every finding of the document
};

}  // namespace pb::detect
