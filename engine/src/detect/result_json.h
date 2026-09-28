// Results as JSON, in the schema of spec/OUTPUT.md. Detected values appear only when `reveal` is set; otherwise they
// are masked (detect/masking.h).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/json.h"
#include "detect/document.h"

namespace pb::detect {

struct JsonOptions {
    bool reveal = false;
};

// One document's generic result: {"file", "input", "bytes", "windows", ..., "decisions", "labels", "scores",
// "spans": [{"type", "start", "end", "confidence", "votes"}], "fields", "byte_map", "per_model", "counters", "note"}.
// `input` is the index of the caller's input the document comes from (an archive yields several documents).
void write_result(json::Writer& w, const DocumentResult& result, const std::vector<uint8_t>& bytes, size_t input,
                  const JsonOptions& o);

// The flat finding list of spec/OUTPUT.md, one object per final span: {"file", "kind", "severity", "start", "end",
// "line", "col", "end_line", "confidence", "votes", "snippet_masked", "context_masked" | "string_masked", ...}.
void write_findings(json::Writer& w, const DocumentResult& result, const std::vector<uint8_t>& bytes,
                    const JsonOptions& o);

}  // namespace pb::detect
