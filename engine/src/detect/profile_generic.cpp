// Profiles `none` and `redact`: the model's outputs with no task-specific rules.
//
// Spans of every window are resolved into typed, non-overlapping spans that cover exactly what the windows marked
// (detect/spans.h); an ensemble keeps a span when enough members mark an overlapping span of the same type. For
// query-conditioned models, each span is also reported as a field: the query whose text appears last before it on
// its line.
//
// `redact` differs in one point: it evaluates inputs of any length, even under the model's shortest window, because
// a short input that is not analyzed would be copied out unredacted.
// Both report every finding as an error: they make no assumption about paths (a redacted copy must hide every value,
// wherever its input comes from).
#include <algorithm>
#include <map>
#include <utility>

#include "detect/common.h"

namespace pb::detect {

namespace {

int64_t type_of(const Finding& f) { return f.type; }

// Ensemble vote on typed spans: in (start, member) order, a span not overlapping an already kept span of the same
// type is kept when at least `votes` distinct members have an overlapping span of that type. The kept spans of a type
// start in that order, and the members' spans are indexed by type: O(F log F), with the results of comparing every
// pair.
std::vector<Finding> vote_by_overlap(const std::vector<Finding>& all, int votes) {
    std::vector<size_t> order(all.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) {
        return all[x].start != all[y].start ? all[x].start < all[y].start : all[x].model < all[y].model;
    });
    std::vector<std::pair<int64_t, int64_t>> spans;
    spans.reserve(all.size());
    for (const Finding& f : all) spans.push_back({f.start, f.end});
    const auto index = member_index(all, spans, type_of);
    std::map<int32_t, MemberIntervals> kept;  // per type: the kept spans, starts in order, with the furthest end
    std::vector<Finding> out;
    for (size_t i : order) {
        const Finding& f = all[i];
        const auto k = kept.find(f.type);
        if (k != kept.end() && k->second.meets(f.start, f.end)) continue;  // overlaps a kept span of its type
        const int members = members_meeting(index.at(f.type), f.start, f.end);
        if (members >= votes) {
            out.push_back(f);
            out.back().votes = members;
            MemberIntervals& t = kept[f.type];
            t.firsts.push_back(f.start);
            t.max_second.push_back(t.max_second.empty() ? f.end : std::max(t.max_second.back(), f.end));
        }
    }
    return out;
}

// Every position where `q` occurs in `b`, overlapping ones included, in order (Knuth-Morris-Pratt: O(|b| + |q|)).
std::vector<int64_t> occurrences(const std::vector<uint8_t>& b, const std::string& q) {
    std::vector<int64_t> out;
    const size_t m = q.size();
    if (m == 0 || m > b.size()) return out;
    std::vector<size_t> border(m, 0);
    for (size_t i = 1, k = 0; i < m; ++i) {
        while (k > 0 && q[i] != q[k]) k = border[k - 1];
        if (q[i] == q[k]) ++k;
        border[i] = k;
    }
    for (size_t i = 0, k = 0; i < b.size(); ++i) {
        const char c = static_cast<char>(b[i]);
        while (k > 0 && c != q[k]) k = border[k - 1];
        if (c == q[k]) ++k;
        if (k == m) {
            out.push_back(static_cast<int64_t>(i + 1 - m));
            k = border[k - 1];
        }
    }
    return out;
}

// Each finding's field: the query whose last occurrence on the finding's line before it (entirely before its start)
// comes last, the first of the queries on a tie. Every occurrence of every query is found once, so a finding costs
// O(queries x log n), not a search of its line.
std::vector<Field> fields_of(const std::vector<Finding>& findings, const std::vector<uint8_t>& b,
                             const LineIndex& lines, const std::vector<std::string>& queries) {
    std::vector<std::vector<int64_t>> found(queries.size());
    for (size_t q = 0; q < queries.size(); ++q) found[q] = occurrences(b, queries[q]);
    std::vector<Field> out;
    for (const Finding& f : findings) {
        const int64_t line_start = lines.line_start(f.start);
        Field field;
        int64_t best = -1;
        for (size_t q = 0; q < queries.size(); ++q) {
            const int64_t length = static_cast<int64_t>(queries[q].size());
            if (length == 0) continue;
            auto it = std::upper_bound(found[q].begin(), found[q].end(), f.start - length);  // ends by f.start
            if (it == found[q].begin() || *(it - 1) < line_start) continue;
            if (best < 0 || *(it - 1) > best) {
                best = *(it - 1);
                field.name = queries[q];
            }
        }
        field.start = f.start;
        field.end = f.end;
        field.confidence = f.confidence;
        field.value = f.text;
        out.push_back(std::move(field));
    }
    return out;
}

class GenericProfile final : public Profile {
public:
    GenericProfile(const char* name, bool any_length) : name_(name), any_length_(any_length) {}
    const char* name() const override { return name_; }

    std::vector<DocumentResult> run(Session& session, const DetectRequest& req,
                                    std::vector<Document>& docs) const override {
        const size_t F = docs.size(), M = req.models.size();
        std::vector<DocumentResult> results(F);
        std::vector<LineIndex> lines;
        lines.reserve(F);
        for (const Document& d : docs) lines.emplace_back(d.bytes);
        const std::vector<std::string> names = entity_names(*req.models[0]);
        const ScanOptions options = scan_options(req, any_length_ ? 1 : 0);
        std::vector<FindingsByMember> per(F, FindingsByMember(M));  // per document
        for (size_t m = 0; m < M; ++m) {
            const Model& model = *req.models[m];
            const ScanResult scanned = scan_documents(session, model, docs, options);
            for (size_t k = 0; k < F; ++k) {
                if (m == 0) summarize(model, scanned.inputs[k], results[k]);
                std::vector<Candidate> spans = window_spans(model, scanned.inputs[k]);
                add_hidden(results[k], spans);
                for (const Candidate& c : resolve(std::move(spans), names)) {
                    Finding f;
                    f.start = c.start;
                    f.end = c.end;
                    f.type = c.type;
                    f.confidence = c.confidence;
                    f.model = static_cast<int>(m);
                    locate(f, docs[k].bytes, lines[k]);
                    per[k][m].push_back(std::move(f));
                }
            }
        }
        for (size_t k = 0; k < F; ++k) {
            conclude(results[k], docs[k], std::move(per[k]), req, vote_by_overlap);
            if (req.models[0]->query.enabled())
                results[k].fields = fields_of(results[k].findings, docs[k].bytes, lines[k], req.queries);
        }
        return results;
    }

private:
    const char* name_;
    bool any_length_;
};

}  // namespace

const Profile& none_profile() {
    static const GenericProfile profile("none", false);
    return profile;
}

const Profile& redact_profile() {
    static const GenericProfile profile("redact", true);
    return profile;
}

}  // namespace pb::detect
