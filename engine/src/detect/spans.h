// Combining spans that come from overlapping windows, several models or several sources.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pb::detect {

struct Candidate {
    int64_t start = 0;
    int64_t end = 0;   // exclusive
    int32_t type = 0;  // entity index (its name comes from the model)
    float confidence = 0.f;
    std::string source = "model";  // who proposed it: "model", "rules", or several joined by '+'
};

// Sorts by (start, end) and merges spans that overlap or are at most `gap` bytes apart. A merged span keeps the
// highest confidence, and the type of the most confident of its parts (the first one on ties).
std::vector<Candidate> merge_nearby(std::vector<Candidate> spans, int64_t gap);

// Typed resolution: the result is sorted, has no overlaps, and covers EXACTLY the union of the input spans.
//   1. spans of the same type that overlap (touching is not overlapping) become one: their union, the higher
//      confidence, the sources joined;
//   2. where spans of different types overlap, the best one owns those bytes (a rule-validated structured type first,
//      then the higher confidence, then the longer span, then the earlier type and position) and the others keep the
//      bytes nobody better claims, possibly in pieces: a byte masked as the wrong type is better than a byte that
//      leaks.
// `type_names` names the types (for the validated-type rule); empty spans are dropped.
std::vector<Candidate> resolve(std::vector<Candidate> spans, const std::vector<std::string>& type_names);

// Types whose rule-based detectors validate a checksum or a strict syntax: a span of one of them from a source that
// includes "rules" outranks the model in step 2 of resolve().
bool is_validated_type(const std::string& name);

}  // namespace pb::detect
