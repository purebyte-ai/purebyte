// C API: detectors (profiles over one model or an ensemble), redaction and archives (see include/purebyte/pb.h).
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>

#include "api/handles.h"
#include "core/json.h"
#include "detect/archive.h"
#include "detect/common.h"
#include "detect/inputs.h"
#include "detect/profile.h"
#include "detect/redaction.h"
#include "detect/result_json.h"

using pb::api::guarded;

struct pb_detector {
    std::vector<std::shared_ptr<const pb::Model>> models;  // held so that the caller may free its handles
    const pb::detect::Profile* profile = nullptr;
};

struct pb_redaction {
    pb::detect::Redaction redaction;
};

struct pb_archive {
    std::vector<pb::detect::ArchiveMember> members;
    std::vector<std::string> failures;  // "name: reason"
};

namespace {

// The smallest struct_size a caller may pass: the struct without its optional last field when it has one
// (pb_detect_options.paths, pb_redact_options.name), which then keeps its default (NULL). A field appended in a later
// version keeps these values: callers built before it pass the smaller size.
constexpr size_t kDetectOptionsV1 = offsetof(pb_detect_options, paths);
constexpr size_t kRedactOptionsV1 = offsetof(pb_redact_options, name);
constexpr size_t kArchiveLimitsV1 = sizeof(pb_archive_limits);

char* copy_string(const std::string& s) {
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (!out) throw std::bad_alloc();
    std::memcpy(out, s.data(), s.size() + 1);
    return out;
}

// The request every call on a detector makes: its models (the ensemble unless told otherwise) and the operating point.
pb::detect::DetectRequest request_of(const pb_detector& d, bool no_ensemble, int32_t votes, int32_t use_bias,
                                     float bias, const float* type_bias, uint32_t type_bias_count,
                                     float min_confidence) {
    pb::detect::DetectRequest r;
    const size_t members = no_ensemble ? 1 : d.models.size();
    for (size_t i = 0; i < members; ++i) r.models.push_back(d.models[i].get());
    if (votes < 0 || votes > static_cast<int32_t>(members))
        pb::fail(PB_ERR_ARGUMENT, pb::format("votes is %d; with %zu model(s) it must be within 0..%zu (0 = a majority)",
                                             static_cast<int>(votes), members, members));
    r.votes = votes;
    r.bias = pb::api::span_bias(use_bias, bias, type_bias, type_bias_count);
    r.min_confidence = min_confidence;
    return r;
}

std::vector<std::string> comma_list(const char* text) {
    std::vector<std::string> out;
    for (const char* p = text; p && *p;) {
        const char* comma = std::strchr(p, ',');
        const std::string item = comma ? std::string(p, comma) : std::string(p);
        if (!item.empty()) out.push_back(item);
        p = comma ? comma + 1 : p + item.size();
    }
    return out;
}

pb::detect::ArchiveLimits archive_limits(const pb_archive_limits& l) {
    pb::detect::ArchiveLimits a;
    a.max_depth = l.max_depth;
    a.max_member_bytes = l.max_member_bytes;
    a.max_total_bytes = l.max_total_bytes;
    a.max_ratio = l.max_ratio;
    return a;
}

}  // namespace

extern "C" {

pb_status pb_detector_create(const pb_model* const* models, size_t model_count, const char* profile, pb_detector** out,
                             pb_error* err) {
    return guarded(err, [&] {
        if (!out || !models || model_count == 0) pb::fail(PB_ERR_ARGUMENT, "a detector needs at least one model");
        *out = nullptr;
        auto d = std::make_unique<pb_detector>();
        for (size_t i = 0; i < model_count; ++i) {
            if (!models[i]) pb::fail(PB_ERR_ARGUMENT, pb::format("model %zu of the detector is null", i));
            d->models.push_back(models[i]->model);
        }
        const std::string declared = d->models[0]->profile;
        const std::string name = profile && *profile ? profile : (declared.empty() ? "none" : declared);
        d->profile = pb::detect::find_profile(name);
        if (!d->profile)
            pb::fail(PB_ERR_UNSUPPORTED,
                     "profile `" + name + "` is not implemented by this version of purebyte (known: " +
                         pb::detect::known_profiles() + "). Update purebyte or choose a known profile.");
        *out = d.release();
    });
}

void pb_detector_free(pb_detector* detector) { delete detector; }
const char* pb_detector_profile(const pb_detector* detector) { return detector ? detector->profile->name() : ""; }

// These return no status, so nothing may escape them: on an exception (no memory for a copy of the name) they give the
// answer that loses nothing (read the file, enter the directory, report an error).
int32_t pb_detector_wants_file(const pb_detector* detector, const char* file_name) {
    try {
        return detector && file_name && detector->profile->wants_file(file_name) ? 1 : 0;
    } catch (...) {
        return 1;
    }
}
int32_t pb_detector_wants_directory(const pb_detector* detector, const char* directory_name) {
    try {
        return detector && directory_name && detector->profile->wants_directory(directory_name) ? 1 : 0;
    } catch (...) {
        return 1;
    }
}
uint64_t pb_detector_max_input_bytes(const pb_detector* detector) {
    return detector ? detector->profile->max_input_bytes() : 0;
}
int32_t pb_detector_path_severity(const pb_detector* detector, const char* path) {
    try {
        return detector && path && detector->profile->path_severity(path) == pb::detect::Severity::warning
                   ? PB_SEVERITY_WARNING
                   : PB_SEVERITY_ERROR;
    } catch (...) {
        return PB_SEVERITY_ERROR;
    }
}

pb_status pb_detect(pb_session* session, const pb_detector* detector, const pb_input* inputs, size_t input_count,
                    const char* const* names, const pb_detect_options* options, char** json_out, pb_error* err) {
    return guarded(err, [&] {
        if (!session || !detector || !json_out || (input_count && !inputs)) pb::fail(PB_ERR_ARGUMENT, "null argument");
        *json_out = nullptr;
        pb_detect_options o;
        pb_detect_options_init(&o);
        pb::api::read_options(options, o, kDetectOptionsV1, "pb_detect_options");
        pb::detect::DetectRequest request =
            request_of(*detector, (o.flags & PB_DETECT_NO_ENSEMBLE) != 0, o.votes, o.use_bias, o.bias, o.type_bias,
                       o.type_bias_count, o.min_confidence);
        request.per_model = (o.flags & PB_DETECT_PER_MODEL) != 0;
        request.keep_examples = (o.flags & PB_DETECT_KEEP_EXAMPLES) != 0;
        request.prefilter = (o.flags & PB_DETECT_PREFILTER) != 0;
        request.strings_only = (o.flags & PB_DETECT_STRINGS_ONLY) != 0;
        request.early_exit = (o.flags & PB_DETECT_EARLY_EXIT) != 0;
        request.queries = pb::api::string_list(o.queries, o.query_count);

        std::vector<pb::detect::Input> in;
        for (size_t i = 0; i < input_count; ++i) {
            if (!inputs[i].data && inputs[i].size)
                pb::fail(PB_ERR_ARGUMENT, pb::format("input %zu has a size and no data", i));
            in.push_back({names && names[i] ? names[i] : "", inputs[i].data, inputs[i].size,
                          o.paths && o.paths[i] ? o.paths[i] : ""});
        }
        const pb::detect::ArchiveLimits limits;
        pb::detect::Intake intake =
            pb::detect::take_inputs(in, *detector->profile, (o.flags & PB_DETECT_EXPAND_ARCHIVES) ? &limits : nullptr);
        std::vector<pb::detect::DocumentResult> results =
            detector->profile->run(*session->session, request, intake.documents);
        for (size_t k = 0; k < results.size(); ++k)
            results[k].severity = detector->profile->path_severity(intake.paths[k]);

        const pb::detect::JsonOptions jo{(o.flags & PB_DETECT_REVEAL) != 0};
        pb::json::Writer w;
        w.begin_object()
            .field("profile", detector->profile->name())
            .field("models", static_cast<int64_t>(request.models.size()));
        w.field("votes_needed", static_cast<int64_t>(pb::detect::votes_needed(request, request.models.size())));
        w.key("results").begin_array();
        for (size_t k = 0; k < results.size(); ++k)
            pb::detect::write_result(w, results[k], intake.documents[k].bytes, intake.origin[k], jo);
        w.end_array().key("findings").begin_array();
        for (size_t k = 0; k < results.size(); ++k)
            pb::detect::write_findings(w, results[k], intake.documents[k].bytes, jo);
        w.end_array().key("failures").begin_array();
        for (const pb::detect::ScanFailure& f : intake.failures)
            w.begin_object().field("file", f.file).field("reason", f.reason).end_object();
        w.end_array().end_object();
        *json_out = copy_string(w.str());
    });
}

pb_status pb_redact(pb_session* session, const pb_detector* detector, const uint8_t* input, size_t size,
                    const pb_redact_options* options, pb_redaction** out, pb_error* err) {
    return guarded(err, [&] {
        if (!session || !detector || !out || (size && !input)) pb::fail(PB_ERR_ARGUMENT, "null argument");
        *out = nullptr;
        pb_redact_options o;
        pb_redact_options_init(&o);
        pb::api::read_options(options, o, kRedactOptionsV1, "pb_redact_options");
        const pb::detect::Profile& profile = *detector->profile;
        if (size > profile.max_input_bytes())
            pb::fail(PB_ERR_ARGUMENT, "the input is " + std::to_string(size) + " bytes, over this model's limit of " +
                                          std::to_string(profile.max_input_bytes()) + " bytes: not redacted");
        pb::detect::DetectRequest request =
            request_of(*detector, false, 0, o.use_bias, o.bias, o.type_bias, o.type_bias_count, o.min_confidence);
        // Every profile analyzes the input whatever its length: one it did not analyze would be copied out
        // unredacted, with a report that says nothing is left.
        request.min_length = 1;
        const std::vector<uint8_t> original(input, input + size);
        std::vector<pb::detect::Document> docs{{"", original}};
        const pb::detect::DocumentResult result = profile.run(*session->session, request, docs)[0];
        if (result.windows == 0 && size > 0)
            pb::fail(PB_ERR_INTERNAL, "the model evaluated no window of the input, so nothing can be redacted");
        const std::vector<std::string> wanted = comma_list(o.types);
        std::vector<pb::detect::RedactionSpan> spans;
        for (const pb::detect::Finding& f : result.findings) {
            const std::string type =
                static_cast<size_t>(f.type) < result.type_names.size() ? result.type_names[f.type] : "SPAN";
            if (!wanted.empty() && std::find(wanted.begin(), wanted.end(), type) == wanted.end()) continue;
            spans.push_back({f.start, f.end, type, f.confidence, "model"});
        }
        pb::detect::RedactionRequest rr;
        rr.input_name = o.name ? o.name : "";
        rr.types = result.type_names;
        rr.model_name = detector->models[0]->name;
        rr.with_map = (o.flags & PB_REDACT_WITH_MAP) != 0;
        auto handle = std::make_unique<pb_redaction>();
        // A profile may have read a converted copy (UTF-16 as UTF-8): the redaction is a copy of the caller's bytes
        // all the same, in their encoding.
        handle->redaction = docs[0].bytes == original
                                ? pb::detect::redact(original, spans, rr)
                                : pb::detect::redact_converted(original, docs[0].bytes, spans, rr);
        *out = handle.release();
    });
}

pb_status pb_redact_spans(const uint8_t* input, size_t size, const pb_redaction_span* spans, size_t span_count,
                          const char* const* type_order, size_t type_count, uint32_t flags, pb_redaction** out,
                          pb_error* err) {
    return guarded(err, [&] {
        if (!out || (size && !input) || (span_count && !spans) || (type_count && !type_order))
            pb::fail(PB_ERR_ARGUMENT, "null argument");
        *out = nullptr;
        std::vector<pb::detect::RedactionSpan> list;
        for (size_t i = 0; i < span_count; ++i) {
            if (!spans[i].type || !*spans[i].type) pb::fail(PB_ERR_ARGUMENT, pb::format("span %zu has no type", i));
            list.push_back({spans[i].start, spans[i].end, spans[i].type, spans[i].confidence,
                            spans[i].source ? spans[i].source : "model"});
        }
        pb::detect::RedactionRequest rr;
        for (size_t i = 0; i < type_count; ++i) rr.types.emplace_back(type_order[i] ? type_order[i] : "");
        rr.with_map = (flags & PB_REDACT_WITH_MAP) != 0;
        auto handle = std::make_unique<pb_redaction>();
        handle->redaction = pb::detect::redact(std::vector<uint8_t>(input, input + size), list, rr);
        *out = handle.release();
    });
}

const uint8_t* pb_redaction_output(const pb_redaction* r, size_t* size) {
    if (size) *size = r ? r->redaction.output.size() : 0;
    return r ? r->redaction.output.data() : nullptr;
}
const char* pb_redaction_report(const pb_redaction* r) { return r ? r->redaction.report_json.c_str() : ""; }
const char* pb_redaction_map(const pb_redaction* r) {
    return r && !r->redaction.map_json.empty() ? r->redaction.map_json.c_str() : nullptr;
}
void pb_redaction_free(pb_redaction* r) { delete r; }

pb_status pb_restore(const uint8_t* redacted, size_t size, const char* map_json, uint8_t** out, size_t* out_size,
                     pb_error* err) {
    return guarded(err, [&] {
        if ((size && !redacted) || !map_json || !out || !out_size) pb::fail(PB_ERR_ARGUMENT, "null argument");
        *out = nullptr;
        *out_size = 0;
        const std::vector<uint8_t> original =
            pb::detect::restore(std::vector<uint8_t>(redacted, redacted + size), map_json);
        uint8_t* buffer = static_cast<uint8_t*>(std::malloc(original.size() ? original.size() : 1));
        if (!buffer) throw std::bad_alloc();
        if (!original.empty()) std::memcpy(buffer, original.data(), original.size());
        *out = buffer;
        *out_size = original.size();
    });
}

int32_t pb_is_archive(const uint8_t* data, size_t size) { return pb::detect::looks_like_archive(data, size) ? 1 : 0; }

pb_status pb_archive_expand(const uint8_t* data, size_t size, const char* name, const pb_archive_limits* limits,
                            pb_archive** out, pb_error* err) {
    return guarded(err, [&] {
        if (!out || (size && !data)) pb::fail(PB_ERR_ARGUMENT, "null argument");
        *out = nullptr;
        pb_archive_limits l;
        pb_archive_limits_init(&l);
        pb::api::read_options(limits, l, kArchiveLimitsV1, "pb_archive_limits");
        if (l.max_depth < 0)
            pb::fail(PB_ERR_ARGUMENT, pb::format("pb_archive_limits.max_depth is %d; it must be 0 or more",
                                                 static_cast<int>(l.max_depth)));
        std::vector<pb::detect::ScanFailure> failures;
        auto a = std::make_unique<pb_archive>();
        a->members = pb::detect::expand_archive(name ? name : "", data, size, archive_limits(l), nullptr, failures);
        for (const pb::detect::ScanFailure& f : failures) a->failures.push_back(f.file + ": " + f.reason);
        *out = a.release();
    });
}

size_t pb_archive_member_count(const pb_archive* a) { return a ? a->members.size() : 0; }
const char* pb_archive_member_name(const pb_archive* a, size_t i) {
    return a && i < a->members.size() ? a->members[i].name.c_str() : nullptr;
}
const uint8_t* pb_archive_member_data(const pb_archive* a, size_t i, size_t* size) {
    if (size) *size = a && i < a->members.size() ? a->members[i].bytes.size() : 0;
    return a && i < a->members.size() ? a->members[i].bytes.data() : nullptr;
}
size_t pb_archive_failure_count(const pb_archive* a) { return a ? a->failures.size() : 0; }
const char* pb_archive_failure(const pb_archive* a, size_t i) {
    return a && i < a->failures.size() ? a->failures[i].c_str() : nullptr;
}
void pb_archive_free(pb_archive* a) { delete a; }

}  // extern "C"
