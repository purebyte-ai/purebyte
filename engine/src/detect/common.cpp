#include "detect/common.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

#include "detect/masking.h"

namespace pb::detect {

ScanOptions scan_options(const DetectRequest& request, int min_length) {
    ScanOptions o;
    o.min_length = min_length > 0 ? min_length : request.min_length;
    o.prefilter = request.prefilter;
    o.early_exit = request.early_exit;
    o.bias = request.bias;
    o.queries = request.queries;
    return o;
}

ScanResult scan_documents(Session& session, const Model& model, const std::vector<Document>& documents,
                          const ScanOptions& options) {
    std::vector<Bytes> inputs;
    inputs.reserve(documents.size());
    for (const Document& d : documents) inputs.push_back({d.bytes.data(), d.bytes.size()});
    return scan(session, model, inputs, options);
}

std::vector<std::string> entity_names(const Model& model) {
    const int tag = model.find_head("tag");
    return tag >= 0 ? model.heads[tag]->labels() : std::vector<std::string>();
}

std::vector<Candidate> window_spans(const Model& model, const std::vector<WindowResult>& windows) {
    std::vector<Candidate> out;
    const int tag = model.find_head("tag");
    if (tag < 0) return out;
    for (const WindowResult& w : windows)
        if (!w.heads.empty())
            for (const Span& s : w.heads[tag].spans) out.push_back({s.start, s.end, s.type, s.confidence, "model"});
    return out;
}

void add_hidden(DocumentResult& result, const std::vector<Candidate>& spans, bool base64) {
    for (const Candidate& c : spans) result.hidden.push_back({c.start, c.end, base64});
}

namespace {

// The window whose first choice-like decision is the most positive: for `max` and `any`. Windows the prefilter
// skipped or the exit head stopped have no head outputs. A value that is not a finite number (only overflow in a
// malformed model produces one) is never the most positive; when no window has a finite one, the first window that
// computed the head is taken. The caller guarantees that some window computed it.
size_t most_positive(const std::vector<WindowResult>& windows, int head) {
    size_t best = windows.size(), first = windows.size();
    float top = -1.f;
    for (size_t i = 0; i < windows.size(); ++i) {
        if (windows[i].heads.empty()) continue;
        const HeadOutput& o = windows[i].heads[head];
        if (!o.computed || o.values.empty()) continue;
        if (first == windows.size()) first = i;
        const float positive = 1.f - o.values[0];
        if (std::isfinite(positive) && positive > top) {
            top = positive;
            best = i;
        }
    }
    return best < windows.size() ? best : first;
}

HeadSummary summarize_head(const Head& h, const std::vector<WindowResult>& windows) {
    HeadSummary s;
    s.head = h.index();
    s.head_name = h.name();
    s.type = h.type();
    s.aggregate = h.aggregate();
    s.names = h.labels();
    std::vector<const HeadOutput*> computed;
    for (const WindowResult& w : windows)
        if (!w.heads.empty() && w.heads[h.index()].computed) computed.push_back(&w.heads[h.index()]);
    if (computed.empty()) return s;
    s.computed = true;
    const size_t width = computed[0]->values.size();
    auto mean = [&]() {
        std::vector<double> sum(width, 0.0);
        for (const HeadOutput* o : computed)
            for (size_t k = 0; k < width; ++k) sum[k] += o->values[k];
        std::vector<float> out(width);
        for (size_t k = 0; k < width; ++k) out[k] = static_cast<float>(sum[k] / computed.size());
        return out;
    };
    const std::string& a = s.aggregate;
    if (s.type == "choice") {
        if (a == "mean") {
            s.values = mean();
            s.label = static_cast<int32_t>(std::max_element(s.values.begin(), s.values.end()) - s.values.begin());
        } else if (a == "vote") {
            std::vector<int> count(width, 0);
            for (const HeadOutput* o : computed) ++count[o->label];
            s.label = static_cast<int32_t>(std::max_element(count.begin(), count.end()) - count.begin());
            s.values.assign(width, 0.f);
            for (size_t k = 0; k < width; ++k) s.values[k] = static_cast<float>(count[k]) / computed.size();
        } else {  // max, any: the most positive window
            const size_t best = most_positive(windows, h.index());
            if (best >= windows.size()) {  // no window has a value (a choice head always has one): nothing to report
                s.computed = false;
                return s;
            }
            const HeadOutput& o = windows[best].heads[h.index()];
            s.values = o.values;
            s.label = o.label;
        }
    } else if (s.type == "multilabel") {
        if (a == "mean") {
            s.values = mean();
        } else {  // max, any
            s.values.assign(width, 0.f);
            for (const HeadOutput* o : computed)
                for (size_t k = 0; k < width; ++k) s.values[k] = std::max(s.values[k], o->values[k]);
        }
        const std::vector<float> thresholds = h.thresholds();
        for (size_t k = 0; k < width; ++k) s.on.push_back(s.values[k] >= thresholds[k]);
    } else if (s.type == "score") {
        if (a == "mean") {
            s.values = mean();
        } else {
            s.values = computed[0]->values;
            for (const HeadOutput* o : computed)
                for (size_t k = 0; k < width; ++k)
                    s.values[k] =
                        a == "max" ? std::max(s.values[k], o->values[k]) : std::min(s.values[k], o->values[k]);
        }
    } else if (s.type == "ordinal") {
        if (a == "mean") {
            s.values = mean();
            s.label = 0;
            for (float p : s.values) s.label += p > 0.5f;
        } else {  // max: the highest level of any window
            const HeadOutput* top = computed[0];
            for (const HeadOutput* o : computed)
                if (o->label > top->label) top = o;
            s.values = top->values;
            s.label = top->label;
        }
    }
    return s;
}

}  // namespace

void summarize(const Model& model, const std::vector<WindowResult>& windows, DocumentResult& out) {
    out.windows = static_cast<int64_t>(windows.size());
    for (const WindowResult& w : windows) {
        out.windows_skipped += (w.flags & kWindowSkipped) != 0;
        out.windows_exited += (w.flags & kWindowExited) != 0;
        out.positive_windows += w.label != 0;
        out.max_positive = std::max(out.max_positive, w.p_positive);
    }
    for (const auto& h : model.heads) {
        const std::string type = h->type();
        if (type == "choice" || type == "multilabel" || type == "score" || type == "ordinal") {
            out.heads.push_back(summarize_head(*h, windows));
        } else if (type == "byte_map") {
            // Each byte takes the label of the earliest window that covers it: the one with the most left context.
            ByteMapSummary m;
            m.head = h->index();
            m.head_name = h->name();
            m.names = h->labels();
            int64_t covered = 0;
            for (const WindowResult& w : windows) {
                if (w.heads.empty() || w.heads[h->index()].byte_labels.empty()) continue;
                const std::vector<uint8_t>& labels = w.heads[h->index()].byte_labels;
                // 0: a byte_map head labels the document bytes of a window only (a query prefix gets none).
                const int64_t offset = static_cast<int64_t>(labels.size()) - w.length;
                for (int64_t p = std::max(covered, w.start); p < w.start + w.length; ++p) {
                    const int32_t label = labels[static_cast<size_t>(p - w.start + offset)];
                    if (!m.regions.empty() && m.regions.back().end == p && m.regions.back().label == label)
                        ++m.regions.back().end;
                    else
                        m.regions.push_back({p, p + 1, label});
                }
                covered = std::max(covered, w.start + w.length);
            }
            out.byte_maps.push_back(std::move(m));
        }
    }
}

void locate(Finding& f, const std::vector<uint8_t>& bytes, const LineIndex& lines) {
    int64_t line_start, line_end;
    lines.locate(f.start, f.line, f.col, line_start, line_end);
    f.text = text_of(bytes, f.start, f.end, &f.text_code_points);
    f.end_line = f.line;
    for (int64_t i = f.start; i < f.end; ++i) f.end_line += bytes[i] == '\n';
    f.multiline = f.end_line > f.line;
    const Range context = context_range(bytes, f.start, f.end, line_start, line_end);
    f.context = strip(text_of(bytes, context.first, context.second));
}

int votes_needed(const DetectRequest& request, size_t members) {
    return request.votes > 0 ? request.votes : static_cast<int>(members / 2 + 1);
}

bool MemberIntervals::meets(int64_t x, int64_t y) const {
    const size_t below = static_cast<size_t>(std::lower_bound(firsts.begin(), firsts.end(), y) - firsts.begin());
    return below > 0 && max_second[below - 1] > x;
}

std::map<int64_t, std::vector<MemberIntervals>> member_index(const std::vector<Finding>& all,
                                                             const std::vector<std::pair<int64_t, int64_t>>& spans,
                                                             int64_t (*key)(const Finding&)) {
    std::vector<size_t> order(all.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t x, size_t y) {
        const int64_t kx = key(all[x]), ky = key(all[y]);
        if (kx != ky) return kx < ky;
        if (all[x].model != all[y].model) return all[x].model < all[y].model;
        return spans[x].first < spans[y].first;
    });
    std::map<int64_t, std::vector<MemberIntervals>> index;
    for (size_t i : order) {
        std::vector<MemberIntervals>& members = index[key(all[i])];
        if (members.empty() || members.back().model != all[i].model) members.push_back({all[i].model, {}, {}});
        MemberIntervals& m = members.back();
        m.firsts.push_back(spans[i].first);
        m.max_second.push_back(m.max_second.empty() ? spans[i].second : std::max(m.max_second.back(), spans[i].second));
    }
    return index;
}

int members_meeting(const std::vector<MemberIntervals>& members, int64_t x, int64_t y) {
    int n = 0;
    for (const MemberIntervals& m : members) n += m.meets(x, y);
    return n;
}

namespace {

int64_t line_of(const Finding& f) { return f.line; }

}  // namespace

// In (line, column, member) order, a finding is a duplicate when it meets an already kept finding of its line: those
// were met earlier, so they start at or before it, and it meets them exactly when the furthest end among them lies
// after its start. Otherwise the distinct members with a finding meeting it on its line are counted. The results are
// those of comparing every pair, in O(F log F).
std::vector<Finding> vote_by_line(const std::vector<Finding>& all, int votes) {
    auto columns = [](const Finding& f) {
        return std::make_pair(
            f.col, f.col + (f.multiline ? 1000000 : std::max<int64_t>(1, static_cast<int64_t>(f.text_code_points))));
    };
    std::vector<std::pair<int64_t, int64_t>> spans;
    spans.reserve(all.size());
    for (const Finding& f : all) spans.push_back(columns(f));
    const auto index = member_index(all, spans, line_of);
    std::vector<size_t> order(all.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) {
        const Finding &p = all[x], &q = all[y];
        if (p.line != q.line) return p.line < q.line;
        if (p.col != q.col) return p.col < q.col;
        return p.model < q.model;
    });
    std::vector<Finding> out;
    bool kept_on_line = false;
    int64_t line = 0, kept_end = 0;  // the furthest end of the findings kept on `line`
    for (size_t i : order) {
        const Finding& f = all[i];
        const auto c = spans[i];
        if (!kept_on_line || f.line != line) {
            kept_on_line = false;
            line = f.line;
        }
        if (kept_on_line && c.first < kept_end) continue;  // meets a kept finding (c.first < c.second always)
        const int members = members_meeting(index.at(f.line), c.first, c.second);
        if (members >= votes) {
            out.push_back(f);
            out.back().votes = members;
            kept_end = kept_on_line ? std::max(kept_end, c.second) : c.second;
            kept_on_line = true;
        }
    }
    return out;
}

std::vector<Finding> vote_by_offset(const std::vector<Finding>& all, int votes) {
    std::vector<size_t> order(all.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) { return all[x].start < all[y].start; });
    std::vector<Finding> out;
    for (size_t i = 0; i < order.size();) {
        size_t j = i;
        std::set<int> members;
        while (j < order.size() && all[order[j]].start == all[order[i]].start) members.insert(all[order[j++]].model);
        if (static_cast<int>(members.size()) >= votes) {
            out.push_back(all[order[i]]);
            out.back().votes = static_cast<int>(members.size());
        }
        i = j;
    }
    return out;
}

void conclude(DocumentResult& r, const Document& d, FindingsByMember members, const DetectRequest& req, Vote vote) {
    r.name = d.name;
    r.bytes = static_cast<int64_t>(d.bytes.size());
    r.type_names = entity_names(*req.models[0]);
    if (members.size() > 1) {
        std::vector<Finding> all;
        for (const std::vector<Finding>& m : members) all.insert(all.end(), m.begin(), m.end());
        r.findings = vote(all, votes_needed(req, members.size()));
    } else {
        r.findings = members[0];
    }
    if (req.min_confidence > 0.f)
        r.findings.erase(std::remove_if(r.findings.begin(), r.findings.end(),
                                        [&](const Finding& f) { return f.confidence < req.min_confidence; }),
                         r.findings.end());
    // No view of the findings may show in clear what any member found, reported or not (the profile added the spans
    // of every window already).
    for (const std::vector<Finding>& m : members)
        for (const Finding& f : m) r.hidden.push_back({f.start, f.end, f.inside_base64});
    merge_hidden(r.hidden);
    if (req.per_model) r.per_model = std::move(members);
    if (r.windows == 0 && r.bytes > 0) r.note = "the input is shorter than the model's shortest window: not analyzed";
}

}  // namespace pb::detect
