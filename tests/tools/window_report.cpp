// window_report: one JSON line per window, in the format of the reference engine's window mode, so that the output of
// this library can be compared line for line (and, with --digest, bit for bit on the hidden states) with the
// reference runs of tests/local/check_exactness.py.
//
//   window_report MODEL (INPUT | @LIST) --window=N --stride=N [--threads=N] [--kernel=K] [--intra-threads=N]
//                 [--prefilter] [--early-exit] [--digest]
//
// Line format:
//   {"f":F,"start":S,"len":N,"done":I,"total":M,"label":L,"bias":B,"spans":[[a,z,e],...][,"h_fnv":"..."][,"end":true]}
// skipped windows end in "prefilter":"skip", windows stopped by the exit head in "exit":LAYER, and inputs without a
// window (shorter than 24 bytes, or unreadable) print {"f":F,"done":0,"total":0,"spans":[]}.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "core/files.h"
#include "core/json.h"
#include "purebyte/pb.h"
#include "runtime/windows.h"

namespace {

[[noreturn]] void die(const std::string& message) {
    std::fprintf(stderr, "window_report: %s\n", message.c_str());
    std::exit(2);
}

int option_int(const char* arg, const char* name, int fallback) {
    const size_t n = std::strlen(name);
    return std::strncmp(arg, name, n) == 0 ? std::atoi(arg + n) : fallback;
}

// The operating bias the tag head uses when the caller gives none, read from the model's description.
float operating_bias(const pb_model* model) {
    pb::json::Value d;
    std::string error;
    if (!pb::json::parse(pb_model_describe(model), d, error)) die("bad model description: " + error);
    for (const pb::json::Value& h : d.get("heads")->array)
        if (h.get("type")->string == "tag") return static_cast<float>(h.get("operating_bias")->number);
    return 0.f;
}

int exit_layer(const pb_model* model) {
    pb::json::Value d;
    std::string error;
    pb::json::parse(pb_model_describe(model), d, error);
    for (const pb::json::Value& h : d.get("heads")->array)
        if (h.get("type")->string == "exit") return static_cast<int>(h.get("layer")->number);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) die("usage: window_report MODEL (INPUT | @LIST) --window=N --stride=N [options]");
    int window = 512, stride = 0, threads = 1, intra = 0;
    std::string kernel = "auto";
    uint32_t flags = 0;
    for (int i = 3; i < argc; ++i) {
        const char* a = argv[i];
        if (!std::strncmp(a, "--window=", 9))
            window = option_int(a, "--window=", 512);
        else if (!std::strncmp(a, "--stride=", 9))
            stride = option_int(a, "--stride=", 0);
        else if (!std::strncmp(a, "--threads=", 10))
            threads = option_int(a, "--threads=", 1);
        else if (!std::strncmp(a, "--intra-threads=", 16))
            intra = option_int(a, "--intra-threads=", 0);
        else if (!std::strncmp(a, "--kernel=", 9))
            kernel = a + 9;
        else if (!std::strcmp(a, "--prefilter"))
            flags |= PB_SCAN_PREFILTER;
        else if (!std::strcmp(a, "--early-exit"))
            flags |= PB_SCAN_EARLY_EXIT;
        else if (!std::strcmp(a, "--digest"))
            flags |= PB_SCAN_DIGEST;
        else
            die(std::string("unknown option ") + a);
    }
    if (stride <= 0) stride = window / 2;

    pb_error err;
    pb_model* model = nullptr;
    if (pb_model_load(argv[1], &model, &err) != PB_OK) die(err.message);
    pb_session_options so;
    pb_session_options_init(&so);
    so.threads = threads;
    so.intra_threads = intra;
    so.kernel = kernel.c_str();
    pb_session* session = nullptr;
    if (pb_session_create(&so, &session, &err) != PB_OK) die(err.message);

    const bool list = argv[2][0] == '@';
    std::vector<std::string> paths;
    if (list) {
        std::ifstream in(argv[2] + 1);
        if (!in) die("cannot open the list");
        for (std::string line; std::getline(in, line);)
            if (!line.empty()) paths.push_back(line);
    } else {
        paths.push_back(argv[2]);
    }
    std::vector<std::vector<uint8_t>> contents(paths.size());
    std::vector<bool> unreadable(paths.size(), false);
    std::vector<pb_input> inputs(paths.size());
    for (size_t k = 0; k < paths.size(); ++k) {
        try {
            contents[k] = pb::read_file(paths[k]);
        } catch (...) {
            unreadable[k] = true;
        }
        inputs[k] = {contents[k].data(), contents[k].size()};
    }

    pb_scan_options opts;
    pb_scan_options_init(&opts);
    opts.window = static_cast<uint32_t>(window);
    opts.stride = static_cast<uint32_t>(stride);
    opts.flags = flags;
    pb_scan_result* result = nullptr;
    if (pb_scan(session, model, inputs.data(), inputs.size(), &opts, &result, &err) != PB_OK) die(err.message);

    const float bias = operating_bias(model);
    const int layer = exit_layer(model);
    std::string out;
    char buf[256];
    for (size_t k = 0; k < paths.size(); ++k) {
        const std::string file = list ? "\"f\":" + std::to_string(k) + "," : "";
        const pb_window* windows = nullptr;
        const pb_span* spans = nullptr;
        const size_t n = pb_scan_result_windows(result, k, &windows);
        pb_scan_result_spans(result, k, &spans);
        if (n == 0) {
            out += "{" + file + "\"done\":0,\"total\":0,\"spans\":[]" +
                   (unreadable[k] ? ",\"error\":\"could not be read\"" : "") + (list ? ",\"end\":true" : "") + "}\n";
            continue;
        }
        const long long total = pb::nominal_window_count(static_cast<int64_t>(contents[k].size()), window, stride);
        for (size_t w = 0; w < n; ++w) {
            const pb_window& win = windows[w];
            std::snprintf(
                buf, sizeof buf,
                "{%s\"start\":%lld,\"len\":%d,\"done\":%zu,\"total\":%lld,\"label\":%d,\"bias\":%.3f,\"spans\":[",
                file.c_str(), static_cast<long long>(win.start), win.length, w + 1, total,
                (win.flags & (PB_WINDOW_SKIPPED | PB_WINDOW_EXITED)) ? 0 : win.label, bias);
            out += buf;
            for (uint32_t s = 0; s < win.span_count; ++s) {
                const pb_span& sp = spans[win.span_first + s];
                std::snprintf(buf, sizeof buf, "%s[%lld,%lld,%d]", s ? "," : "", static_cast<long long>(sp.start),
                              static_cast<long long>(sp.end), sp.type);
                out += buf;
            }
            out += "]";
            if (win.flags & PB_WINDOW_SKIPPED)
                out += ",\"prefilter\":\"skip\"";
            else if (win.flags & PB_WINDOW_EXITED)
                out += ",\"exit\":" + std::to_string(layer);
            else if (flags & PB_SCAN_DIGEST) {
                std::snprintf(buf, sizeof buf, ",\"h_fnv\":\"%016llx\"", static_cast<unsigned long long>(win.digest));
                out += buf;
            }
            if (list && w + 1 == n) out += ",\"end\":true";
            out += "}\n";
        }
    }
    std::fwrite(out.data(), 1, out.size(), stdout);
    pb_scan_result_free(result);
    pb_session_free(session);
    pb_model_free(model);
    return 0;
}
