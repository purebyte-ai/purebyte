// What results show instead of a detected value. Every output path (JSON, SARIF, logs, errors) goes through these
// functions unless the caller explicitly asked to reveal the values.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "detect/document.h"

namespace pb::detect {

// The first line of `text` with its middle hidden: 16+ code points keep 4 at the start and 2 at the end, 8+ keep 2 at
// the start, shorter ones keep none; at most 12 asterisks. A PEM header line of a key ("-----BEGIN RSA PRIVATE
// KEY-----": key-type words only) is shown as it is (it names the key type, not the key). A value that holds line
// feeds gets " … (N lines)", N being one more than its line feeds.
std::string mask(const std::string& text);

// The masked views of the findings of one document, built once for all of them (the work per view is bounded by the
// view, not by the number of findings: a document with 10^5 findings costs O(F log F) in all).
//   * line(f): the line of a text finding (on a long line, its part around the finding: detect/text.h,
//     context_range) with it and every finding of `shown` on that line masked (one inside a base64 run as
//     "[base64 blob, N B]"), stripped and cut at 160 code points.
//   * string(f): the printable string around a binary finding with every finding of `shown` in it masked, cut at 160
//     code points.
// Overlapping findings are masked as one: no byte of any of them is shown. A finding that covers only part of a token
// (letters, digits and + / = _ - . ~) is masked with the whole token, so that no rest of it is shown either. Every
// byte of `hidden` (sorted, disjoint: DocumentResult::hidden) is hidden as well; a part of it that no finding covers
// is masked as a value of its own. The views depend on `shown` only through the findings they meet, in the order of
// `shown`, then `f` itself.
class MaskedViews {
public:
    MaskedViews(const std::vector<uint8_t>& bytes, const std::vector<Finding>& shown,
                const std::vector<HiddenSpan>* hidden = nullptr);
    std::string line(const Finding& f) const;
    std::string string(const Finding& f) const;

private:
    // Indexes of `shown` meeting [lo, hi), in the order of `shown`.
    std::vector<size_t> meeting(int64_t lo, int64_t hi) const;
    void collect(size_t node, size_t l, size_t r, size_t below, int64_t lo, std::vector<size_t>& out) const;
    std::string render(int64_t lo, int64_t hi, const Finding& f, bool ascii) const;

    const std::vector<uint8_t>& bytes_;
    const std::vector<Finding>& shown_;
    const std::vector<HiddenSpan>* hidden_;
    std::vector<size_t> order_;     // `shown` by start (then position in `shown`)
    std::vector<int64_t> starts_;   // their starts, in that order
    std::vector<int64_t> max_end_;  // segment tree over `order_`: the largest end below each node
    size_t leaves_ = 1;
    mutable std::map<std::pair<int64_t, int64_t>, std::string> strings_;  // string(f), by string range
};

// One view, for callers with a handful of findings (tests); MaskedViews for a document's findings.
std::string masked_line(const std::vector<Finding>& others, const Finding& finding, const std::vector<uint8_t>& bytes);
std::string masked_string(const std::vector<Finding>& others, const Finding& finding,
                          const std::vector<uint8_t>& bytes);

// Sorts `spans` and merges those that overlap or touch (a merged stretch is base64 when any of its parts is).
void merge_hidden(std::vector<HiddenSpan>& spans);

}  // namespace pb::detect
