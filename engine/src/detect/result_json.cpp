#include "detect/result_json.h"

#include <cstdio>

#include "detect/masking.h"
#include "detect/text.h"

namespace pb::detect {

namespace {

std::string type_name(const DocumentResult& r, int32_t type) {
    return type >= 0 && static_cast<size_t>(type) < r.type_names.size() ? r.type_names[type]
                                                                        : "entity_" + std::to_string(type);
}

// One finding. Its masked views hide it, every finding of the views that shares its line or string, and whatever a
// model marked there (DocumentResult::hidden); a finding inside a base64 run has no `end_line` (its `line` is the
// run's; the decoded text has lines of its own).
void write_finding(json::Writer& w, const DocumentResult& r, const Finding& f, const MaskedViews& views,
                   const std::vector<uint8_t>& b, const JsonOptions& o) {
    w.begin_object()
        .field("file", r.name)
        .field("kind", type_name(r, f.type))
        .field("severity", severity_name(r.severity))
        .field("start", f.start)
        .field("end", f.end);
    if (f.binary) {
        char hex[24];
        std::snprintf(hex, sizeof hex, "0x%llx", static_cast<unsigned long long>(f.start));
        w.field("offset_hex", hex);
    } else if (f.line > 0) {
        w.field("line", f.line).field("col", f.col);
        if (!f.inside_base64) w.field("end_line", f.end_line);
    }
    w.field("confidence", static_cast<double>(f.confidence)).field("votes", f.votes);
    if (f.inside_base64)
        w.field("inside_base64", true).field("inner_start", f.inner_start).field("inner_end", f.inner_end);
    w.field("snippet_masked", mask(f.text));
    if (f.binary)
        w.field("string_masked", views.string(f));
    else if (f.line > 0)
        w.field("context_masked", views.line(f));
    if (o.reveal) {
        w.field("snippet", f.text);
        if (f.binary)
            w.field("string",
                    ascii_text(b.data() + f.string_start, static_cast<size_t>(f.string_end - f.string_start)));
        else if (f.line > 0)
            w.field("context", f.context);
    }
    w.end_object();
}

}  // namespace

void write_result(json::Writer& w, const DocumentResult& r, const std::vector<uint8_t>& b, size_t input,
                  const JsonOptions& o) {
    w.begin_object().field("file", r.name).field("input", static_cast<int64_t>(input)).field("bytes", r.bytes);
    w.field("windows", r.windows);
    if (r.windows_skipped) w.field("windows_skipped", r.windows_skipped);
    if (r.windows_exited) w.field("windows_exited", r.windows_exited);
    w.field("positive_windows", r.positive_windows);

    w.key("decisions").begin_object();
    for (const HeadSummary& h : r.heads) {
        if (h.type != "choice" || !h.computed) continue;
        w.key(h.head_name).begin_object().field("index", h.label);
        if (static_cast<size_t>(h.label) < h.names.size()) w.field("label", h.names[h.label]);
        w.field("probability", static_cast<double>(h.values[h.label])).key("probabilities").begin_array();
        for (float v : h.values) w.real(v);
        w.end_array().field("aggregate", h.aggregate).end_object();
    }
    w.end_object();

    w.key("labels").begin_object();
    for (const HeadSummary& h : r.heads) {
        if (h.type != "multilabel" || !h.computed) continue;
        w.key(h.head_name).begin_array();
        for (size_t k = 0; k < h.values.size(); ++k) {
            if (!h.on[k]) continue;
            w.begin_object().field("index", static_cast<int64_t>(k));
            if (k < h.names.size()) w.field("label", h.names[k]);
            w.field("probability", static_cast<double>(h.values[k])).end_object();
        }
        w.end_array();
    }
    w.end_object();

    w.key("scores").begin_object();
    for (const HeadSummary& h : r.heads) {
        if ((h.type != "score" && h.type != "ordinal") || !h.computed) continue;
        w.key(h.head_name).begin_object();
        if (h.type == "ordinal") {
            w.field("level", h.label);
            if (static_cast<size_t>(h.label) < h.names.size()) w.field("label", h.names[h.label]);
        }
        w.key("values").begin_array();
        for (float v : h.values) w.real(v);
        w.end_array();
        if (h.type == "score" && !h.names.empty()) {
            w.key("names").begin_array();
            for (const std::string& n : h.names) w.string(n);
            w.end_array();
        }
        w.end_object();
    }
    w.end_object();

    w.key("spans").begin_array();
    for (const Finding& f : r.findings)
        w.begin_object()
            .field("type", type_name(r, f.type))
            .field("start", f.start)
            .field("end", f.end)
            .field("confidence", static_cast<double>(f.confidence))
            .field("votes", f.votes)
            .end_object();
    w.end_array();

    if (!r.fields.empty()) {
        w.key("fields").begin_array();
        for (const Field& f : r.fields) {
            w.begin_object().field("name", f.name).field("start", f.start).field("end", f.end);
            w.field("confidence", static_cast<double>(f.confidence)).field("value_masked", mask(f.value));
            if (o.reveal) w.field("value", f.value);
            w.end_object();
        }
        w.end_array();
    }
    if (!r.byte_maps.empty()) {
        w.key("byte_map").begin_object();
        for (const ByteMapSummary& m : r.byte_maps) {
            w.key(m.head_name).begin_array();
            for (const ByteRegion& region : m.regions) {
                w.begin_object().field("start", region.start).field("end", region.end).field("index", region.label);
                if (static_cast<size_t>(region.label) < m.names.size()) w.field("label", m.names[region.label]);
                w.end_object();
            }
            w.end_array();
        }
        w.end_object();
    }
    if (!r.per_model.empty()) {  // every ensemble member's own findings, in the flat format
        w.key("per_model").begin_array();
        for (const std::vector<Finding>& member : r.per_model) {
            // The views show the reported findings and this member's masked; every other member's findings are in
            // `hidden`, so no view shows any of them in clear either.
            std::vector<Finding> shown = r.findings;
            shown.insert(shown.end(), member.begin(), member.end());
            const MaskedViews views(b, shown, &r.hidden);
            w.begin_array();
            for (const Finding& f : member) write_finding(w, r, f, views, b, o);
            w.end_array();
        }
        w.end_array();
    }
    if (!r.counters.empty()) {
        w.key("counters").begin_object();
        for (const auto& kv : r.counters) w.field(kv.first, kv.second);
        w.end_object();
    }
    if (!r.note.empty()) w.field("note", r.note);
    w.end_object();
}

void write_findings(json::Writer& w, const DocumentResult& r, const std::vector<uint8_t>& b, const JsonOptions& o) {
    const MaskedViews views(b, r.findings, &r.hidden);
    for (const Finding& f : r.findings) write_finding(w, r, f, views, b, o);
}

}  // namespace pb::detect
