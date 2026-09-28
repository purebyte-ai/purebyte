// C API: versions, models, sessions, window scans and encoding (see include/purebyte/pb.h).
#include "api/handles.h"
#include "core/files.h"
#include "runtime/encode.h"

using pb::api::guarded;

namespace {

// Sizes of the option structs in ABI version 1, the smallest struct_size a caller may pass. They are the current sizes
// only while no field has been appended: write them as numbers before appending one.
constexpr size_t kSessionOptionsV1 = sizeof(pb_session_options);
constexpr size_t kScanOptionsV1 = sizeof(pb_scan_options);

}  // namespace

namespace pb::api {

SpanBias span_bias(int32_t use_bias, float bias, const float* type_bias, uint32_t type_bias_count) {
    SpanBias b;
    b.has_scalar = use_bias != 0;
    b.scalar = bias;
    if (type_bias && type_bias_count)
        b.per_type.assign(type_bias, type_bias + type_bias_count);
    else if (type_bias_count)
        fail(PB_ERR_ARGUMENT, "type_bias_count is set but type_bias is null");
    return b;
}

std::vector<std::string> string_list(const char* const* items, uint32_t count) {
    std::vector<std::string> out;
    if (count && !items) fail(PB_ERR_ARGUMENT, "a list count is set but the list is null");
    for (uint32_t i = 0; i < count; ++i) {
        if (!items[i]) fail(PB_ERR_ARGUMENT, "null string in a list");
        out.emplace_back(items[i]);
    }
    return out;
}

}  // namespace pb::api

extern "C" {

uint32_t pb_abi_version(void) { return PB_ABI_VERSION; }
const char* pb_version(void) { return PUREBYTE_VERSION; }
uint32_t pb_format_version(void) { return static_cast<uint32_t>(pb::kFormatVersion); }

const char* pb_status_name(pb_status status) {
    switch (status) {
        case PB_OK: return "ok";
        case PB_ERR_ARGUMENT: return "argument";
        case PB_ERR_IO: return "io";
        case PB_ERR_FORMAT: return "format";
        case PB_ERR_UNSUPPORTED: return "unsupported";
        case PB_ERR_NO_MEMORY: return "no_memory";
        case PB_ERR_BUFFER_TOO_SMALL: return "buffer_too_small";
        case PB_ERR_INTERNAL: return "internal";
    }
    return "unknown";
}

void pb_free(void* memory) { std::free(memory); }

pb_status pb_model_load(const char* path, pb_model** out, pb_error* err) {
    return guarded(err, [&] {
        if (!path || !out) pb::fail(PB_ERR_ARGUMENT, "path and out must not be null");
        *out = nullptr;
        auto handle = std::make_unique<pb_model>();
        handle->model = pb::load_model(path);
        *out = handle.release();
    });
}

pb_status pb_model_load_memory(const void* data, size_t size, pb_model** out, pb_error* err) {
    return guarded(err, [&] {
        if (!data || !out) pb::fail(PB_ERR_ARGUMENT, "data and out must not be null");
        *out = nullptr;
        auto handle = std::make_unique<pb_model>();
        handle->model = pb::load_model_memory(data, size);
        *out = handle.release();
    });
}

void pb_model_free(pb_model* model) { delete model; }

const char* pb_model_describe(const pb_model* model) { return model ? model->model->description.c_str() : ""; }
int32_t pb_model_d_model(const pb_model* model) { return model ? model->model->d_model : 0; }
int32_t pb_model_head_count(const pb_model* model) {
    return model ? static_cast<int32_t>(model->model->heads.size()) : 0;
}

int32_t pb_model_find_head(const pb_model* model, const char* type) {
    try {  // no status to return: nothing may escape (no memory for a copy of `type` means no head found)
        return model && type ? model->model->find_head(type) : -1;
    } catch (...) {
        return -1;
    }
}

pb_status pb_session_create(const pb_session_options* options, pb_session** out, pb_error* err) {
    return guarded(err, [&] {
        if (!out) pb::fail(PB_ERR_ARGUMENT, "out must not be null");
        *out = nullptr;
        pb_session_options o;
        pb_session_options_init(&o);
        pb::api::read_options(options, o, kSessionOptionsV1, "pb_session_options");
        auto handle = std::make_unique<pb_session>();
        handle->session = std::make_unique<pb::Session>(o.threads, o.intra_threads, o.kernel ? o.kernel : "auto");
        *out = handle.release();
    });
}

void pb_session_free(pb_session* session) { delete session; }
const char* pb_session_kernel(const pb_session* session) { return session ? session->session->kernels().name : ""; }
int32_t pb_session_threads(const pb_session* session) { return session ? session->session->threads() : 0; }

pb_status pb_scan(pb_session* session, const pb_model* model, const pb_input* inputs, size_t input_count,
                  const pb_scan_options* options, pb_scan_result** out, pb_error* err) {
    return guarded(err, [&] {
        if (!session || !model || !out || (input_count && !inputs)) pb::fail(PB_ERR_ARGUMENT, "null argument");
        *out = nullptr;
        pb_scan_options o;
        pb_scan_options_init(&o);
        pb::api::read_options(options, o, kScanOptionsV1, "pb_scan_options");
        // Values the runtime cannot hold are refused, not wrapped into another value.
        constexpr uint32_t kMaxWindow = uint32_t(1) << 30;  // the longest input run as one window
        if (o.window > kMaxWindow || o.stride > kMaxWindow)
            pb::fail(PB_ERR_ARGUMENT,
                     pb::format("window %u / stride %u: at most %u bytes", o.window, o.stride, kMaxWindow));
        pb::ScanOptions so;
        so.window = static_cast<int>(o.window);
        so.stride = static_cast<int>(o.stride);
        so.whole = (o.flags & PB_SCAN_WHOLE) != 0;
        so.prefilter = (o.flags & PB_SCAN_PREFILTER) != 0;
        if (o.prefilter_rule) so.prefilter_rule = o.prefilter_rule;
        so.early_exit = (o.flags & PB_SCAN_EARLY_EXIT) != 0;
        so.digest = (o.flags & PB_SCAN_DIGEST) != 0;
        so.ungated = (o.flags & PB_SCAN_UNGATED) != 0;
        so.bias = pb::api::span_bias(o.use_bias, o.bias, o.type_bias, o.type_bias_count);
        so.queries = pb::api::string_list(o.queries, o.query_count);
        std::vector<pb::Bytes> in(input_count);
        for (size_t i = 0; i < input_count; ++i) {
            if (!inputs[i].data && inputs[i].size) pb::fail(PB_ERR_ARGUMENT, "input with a size and no data");
            in[i] = {inputs[i].data, inputs[i].size};
        }
        auto handle = std::make_unique<pb_scan_result>();
        handle->result = pb::scan(*session->session, *model->model, in, so);
        const int tag = model->model->find_head("tag");
        handle->windows.resize(input_count);
        handle->spans.resize(input_count);
        for (size_t i = 0; i < input_count; ++i)
            for (const pb::WindowResult& w : handle->result.inputs[i]) {
                pb_window pw{};
                pw.start = w.start;
                pw.length = w.length;
                pw.label = w.label;
                pw.p_positive = w.p_positive;
                pw.flags = w.flags;
                pw.digest = w.digest;
                pw.span_first = static_cast<uint32_t>(handle->spans[i].size());
                if (tag >= 0 && !w.heads.empty())
                    for (const pb::Span& s : w.heads[tag].spans)
                        handle->spans[i].push_back({s.start, s.end, s.type, s.confidence});
                pw.span_count = static_cast<uint32_t>(handle->spans[i].size()) - pw.span_first;
                handle->windows[i].push_back(pw);
            }
        *out = handle.release();
    });
}

void pb_scan_result_free(pb_scan_result* result) { delete result; }

size_t pb_scan_result_windows(const pb_scan_result* result, size_t input, const pb_window** windows) {
    if (!result || input >= result->windows.size()) {
        if (windows) *windows = nullptr;
        return 0;
    }
    if (windows) *windows = result->windows[input].data();
    return result->windows[input].size();
}

size_t pb_scan_result_spans(const pb_scan_result* result, size_t input, const pb_span** spans) {
    if (!result || input >= result->spans.size()) {
        if (spans) *spans = nullptr;
        return 0;
    }
    if (spans) *spans = result->spans[input].data();
    return result->spans[input].size();
}

const float* pb_scan_result_head_values(const pb_scan_result* result, size_t input, size_t window, int32_t head,
                                        size_t* count) {
    if (count) *count = 0;
    if (!result || input >= result->result.inputs.size() || window >= result->result.inputs[input].size())
        return nullptr;
    const pb::WindowResult& w = result->result.inputs[input][window];
    if (head < 0 || static_cast<size_t>(head) >= w.heads.size() || !w.heads[head].computed ||
        w.heads[head].values.empty())
        return nullptr;
    if (count) *count = w.heads[head].values.size();
    return w.heads[head].values.data();
}

const uint8_t* pb_scan_result_byte_map(const pb_scan_result* result, size_t input, size_t window, int32_t head,
                                       size_t* count) {
    if (count) *count = 0;
    if (!result || input >= result->result.inputs.size() || window >= result->result.inputs[input].size())
        return nullptr;
    const pb::WindowResult& w = result->result.inputs[input][window];
    if (head < 0 || static_cast<size_t>(head) >= w.heads.size() || w.heads[head].byte_labels.empty()) return nullptr;
    if (count) *count = w.heads[head].byte_labels.size();
    return w.heads[head].byte_labels.data();
}

pb_status pb_encode(pb_session* session, const pb_model* model, const uint8_t* input, size_t size, float* hidden,
                    size_t hidden_capacity, pb_error* err) {
    return guarded(err, [&] {
        if (!session || !model || !input || !hidden) pb::fail(PB_ERR_ARGUMENT, "null argument");
        const size_t needed = size * static_cast<size_t>(model->model->d_model);
        if (hidden_capacity < needed) pb::fail(PB_ERR_BUFFER_TOO_SMALL, pb::format("hidden needs %zu floats", needed));
        const std::vector<float> h = pb::encode(*session->session, *model->model, {input, size});
        std::memcpy(hidden, h.data(), sizeof(float) * needed);
    });
}

}  // extern "C"
