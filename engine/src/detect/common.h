// Building blocks shared by the profiles: running a model over documents, summarising window-level heads, placing a
// finding on its line, and the ensemble vote.
#pragma once

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "detect/document.h"
#include "detect/profile.h"
#include "detect/spans.h"
#include "detect/text.h"
#include "runtime/scan.h"

namespace pb::detect {

// Scan options of a request; `min_length` > 0 (else the request's, when set) overrides the model's shortest evaluated
// window.
ScanOptions scan_options(const DetectRequest& request, int min_length = 0);

// Runs `model` over the documents (every window of every document in one parallel batch).
ScanResult scan_documents(Session& session, const Model& model, const std::vector<Document>& documents,
                          const ScanOptions& options);

// Window statistics, window-level heads aggregated as each head declares, and byte maps, into `out`.
void summarize(const Model& model, const std::vector<WindowResult>& windows, DocumentResult& out);

// Entity names of the model's first tag head (empty when it has none).
std::vector<std::string> entity_names(const Model& model);

// The spans of the first tag head over all the windows of a document, as candidates.
std::vector<Candidate> window_spans(const Model& model, const std::vector<WindowResult>& windows);

// Adds spans a model marked to `result.hidden`: the masked views hide them whether or not a finding reports them.
void add_hidden(DocumentResult& result, const std::vector<Candidate>& spans, bool base64 = false);

// Line, column, last line, text and line context of a finding in a text document.
void locate(Finding& f, const std::vector<uint8_t>& bytes, const LineIndex& lines);

// Votes needed: the request's, else a majority of `members`.
int votes_needed(const DetectRequest& request, size_t members);

// Findings of one document, one list per ensemble member (member 0 is the main model).
using FindingsByMember = std::vector<std::vector<Finding>>;

// An ensemble vote: every member's findings (member 0's first, then member 1's...) -> the findings kept, each carrying
// the number of members that agree.
using Vote = std::vector<Finding> (*)(const std::vector<Finding>& all, int votes);

// Text findings: findings on the same line whose column ranges overlap are the same finding (a multi-line finding
// counts its whole first line). In (line, column, member) order, a finding not overlapping an already kept one is
// kept when at least `votes` distinct members have an overlapping finding.
std::vector<Finding> vote_by_line(const std::vector<Finding>& all, int votes);

// Binary findings: a finding is its start offset; kept when at least `votes` members report it.
std::vector<Finding> vote_by_offset(const std::vector<Finding>& all, int votes);

// What the votes count, in O(log F) per question instead of a pass over every finding: the intervals [first, second)
// of one member in one group (a line, an entity type), sorted by first, with the largest second of every prefix.
struct MemberIntervals {
    int model = 0;
    std::vector<int64_t> firsts, max_second;
    // True when one of them meets [x, y): it starts before y and ends after x.
    bool meets(int64_t x, int64_t y) const;
};

// For every group key (key(finding)), each member's intervals (`spans[i]` is the interval of `all[i]`).
std::map<int64_t, std::vector<MemberIntervals>> member_index(const std::vector<Finding>& all,
                                                             const std::vector<std::pair<int64_t, int64_t>>& spans,
                                                             int64_t (*key)(const Finding&));
// The number of distinct members with an interval meeting [x, y).
int members_meeting(const std::vector<MemberIntervals>& members, int64_t x, int64_t y);

// The end every profile shares for one document: name and size, the entity names, the ensemble vote (member 0's
// findings alone without an ensemble), every member's findings when asked, the confidence floor, and a note when the
// input was too short to analyze. `members` holds each member's findings on this document; they join the spans the
// profile put in `result.hidden` (every window span of every member, with add_hidden) so that no masked view shows
// any of them.
void conclude(DocumentResult& result, const Document& document, FindingsByMember members, const DetectRequest& request,
              Vote vote);

}  // namespace pb::detect
