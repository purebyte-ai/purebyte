// model_dump: everything the runtime computes for a model over a list of inputs, as one JSON document, so that tests
// can compare it with the NumPy reference (reference/) and with golden files (tests/golden).
//
//   model_dump MODEL INPUTS [--window=N] [--stride=N] [--whole] [--early-exit] [--ungated] [--bias=X]
//              [--type-bias=X,Y,...] [--query=TEXT ...] [--threads=N] [--intra-threads=N] [--kernel=K] [--hidden]
//
// INPUTS is the file format of tests/gen/random_models.py (u32 count, then u32 length + bytes per input). Output:
//   {"model": <pb_model_describe>, "kernel": K, "platform": P,
//    "inputs": [{"size": n,
//                "windows": [{"start", "length", "flags", "label", "p_positive", "digest", "heads": [null |
//                             {"label", "values", "spans": [[start, end, type, confidence]], "byte_map": "<hex>"}]}],
//                "hidden": [[...] per byte]}]}
// "hidden" (with --hidden) holds the final hidden states of the whole input run as one sequence (pb_encode). Every
// float is printed with enough digits to read back the same float.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "core/failure.h"
#include "core/files.h"
#include "core/json.h"
#include "model/model.h"
#include "runtime/encode.h"
#include "runtime/scan.h"
#include "runtime/session.h"

namespace {

[[noreturn]] void die(const std::string& message) {
    std::fprintf(stderr, "model_dump: %s\n", message.c_str());
    std::exit(2);
}

bool option(const std::string& arg, const char* name, std::string& value) {
    const std::string prefix = std::string(name) + "=";
    if (arg.compare(0, prefix.size(), prefix) != 0) return false;
    value = arg.substr(prefix.size());
    return true;
}

std::vector<std::vector<uint8_t>> read_inputs(const std::string& path) {
    const std::vector<uint8_t> blob = pb::read_file(path);
    size_t at = 0;
    auto u32 = [&]() {
        if (at + 4 > blob.size()) die("truncated inputs file");
        uint32_t v;
        std::memcpy(&v, blob.data() + at, 4);
        at += 4;
        return v;
    };
    std::vector<std::vector<uint8_t>> out(u32());
    for (auto& input : out) {
        const uint32_t n = u32();
        if (at + n > blob.size()) die("truncated inputs file");
        input.assign(blob.begin() + static_cast<ptrdiff_t>(at), blob.begin() + static_cast<ptrdiff_t>(at + n));
        at += n;
    }
    return out;
}

void write_floats(pb::json::Writer& w, const float* v, size_t n) {
    w.begin_array();
    for (size_t i = 0; i < n; ++i) w.real(static_cast<double>(v[i]));
    w.end_array();
}

void write_head(pb::json::Writer& w, const pb::HeadOutput& h) {
    if (!h.computed) {
        w.null();
        return;
    }
    static const char* hex = "0123456789abcdef";
    w.begin_object().field("label", static_cast<int64_t>(h.label));
    w.key("values");
    write_floats(w, h.values.data(), h.values.size());
    w.key("spans").begin_array();
    for (const pb::Span& s : h.spans) {
        w.begin_array()
            .integer(s.start)
            .integer(s.end)
            .integer(s.type)
            .real(static_cast<double>(s.confidence))
            .end_array();
    }
    w.end_array();
    std::string map;
    for (uint8_t b : h.byte_labels) {
        map += hex[b >> 4];
        map += hex[b & 15];
    }
    w.field("byte_map", map).end_object();
}

// A fingerprint of the C library's binary32 functions (the only source of differences between platforms, spec/FORMAT.md
// section 5): FNV-1a of expf, logf, log1pf, sinf and cosf over fixed arguments in the ranges the engine uses.
std::string libm_fingerprint() {
    uint64_t h = 1469598103934665603ull;
    uint32_t state = 12345;
    for (int i = 0; i < 4096; ++i) {
        state = state * 1664525u + 1013904223u;
        const float u = static_cast<float>(state >> 8) / 16777216.f;
        const float values[5] = {std::exp(-20.f + 30.f * u), std::log(1e-3f + 50.f * u), std::log1p(1e-4f + 20.f * u),
                                 std::sin(500.f * u), std::cos(500.f * u)};
        const auto* p = reinterpret_cast<const uint8_t*>(values);
        for (size_t k = 0; k < sizeof values; ++k) h = (h ^ p[k]) * 1099511628211ull;
    }
    char hex[20];
    std::snprintf(hex, sizeof hex, "%08llx", static_cast<unsigned long long>(h >> 32));
    return hex;
}

// The platform whose C library computed the results: raw values (and digests) agree bit for bit only between runs
// with the same one (spec/FORMAT.md, section 5).
std::string platform() {
#if defined(_WIN32) && defined(__MINGW32__)
    std::string os = "windows-mingw";
#elif defined(_WIN32)
    std::string os = "windows-msvc";
#elif defined(__APPLE__)
    std::string os = "macos";
#elif defined(__linux__) && defined(__GLIBC__)
    std::string os = "linux-glibc";
#else
    std::string os = "other";
#endif
#if defined(__x86_64__) || defined(_M_X64)
    os += "-x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    os += "-arm64";
#endif
    return os + "/libm-" + libm_fingerprint();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) die("usage: model_dump MODEL INPUTS [options]");
    pb::ScanOptions o;
    int threads = 1, intra = 0;
    std::string kernel = "auto", value;
    bool hidden = false;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (option(a, "--window", value))
            o.window = std::atoi(value.c_str());
        else if (option(a, "--stride", value))
            o.stride = std::atoi(value.c_str());
        else if (option(a, "--threads", value))
            threads = std::atoi(value.c_str());
        else if (option(a, "--intra-threads", value))
            intra = std::atoi(value.c_str());
        else if (option(a, "--kernel", value))
            kernel = value;
        else if (option(a, "--query", value))
            o.queries.push_back(value);
        else if (option(a, "--bias", value)) {
            o.bias.has_scalar = true;
            o.bias.scalar = static_cast<float>(std::atof(value.c_str()));
        } else if (option(a, "--type-bias", value)) {
            for (size_t at = 0; at <= value.size();) {
                const size_t comma = std::min(value.find(',', at), value.size());
                o.bias.per_type.push_back(static_cast<float>(std::atof(value.substr(at, comma - at).c_str())));
                at = comma + 1;
            }
        } else if (a == "--whole")
            o.whole = true;
        else if (a == "--early-exit")
            o.early_exit = true;
        else if (a == "--ungated")
            o.ungated = true;
        else if (a == "--hidden")
            hidden = true;
        else
            die("unknown option " + a);
    }
    o.digest = true;
    try {
        const auto model = pb::load_model(argv[1]);
        const std::vector<std::vector<uint8_t>> inputs = read_inputs(argv[2]);
        pb::Session session(threads, intra, kernel);
        std::vector<pb::Bytes> bytes;
        for (const auto& in : inputs) bytes.push_back({in.data(), in.size()});
        const pb::ScanResult r = pb::scan(session, *model, bytes, o);

        pb::json::Writer w;
        w.begin_object().key("model").raw(model->description).field("kernel", session.kernels().name);
        w.field("platform", platform());
        w.key("inputs").begin_array();
        for (size_t i = 0; i < inputs.size(); ++i) {
            w.begin_object().field("size", static_cast<int64_t>(inputs[i].size()));
            w.key("windows").begin_array();
            for (const pb::WindowResult& win : r.inputs[i]) {
                char digest[20];
                std::snprintf(digest, sizeof digest, "%016llx", static_cast<unsigned long long>(win.digest));
                w.begin_object()
                    .field("start", win.start)
                    .field("length", static_cast<int64_t>(win.length))
                    .field("flags", static_cast<int64_t>(win.flags))
                    .field("label", static_cast<int64_t>(win.label))
                    .field("p_positive", static_cast<double>(win.p_positive))
                    .field("digest", std::string(digest));
                w.key("heads").begin_array();
                for (const pb::HeadOutput& h : win.heads) write_head(w, h);
                w.end_array().end_object();
            }
            w.end_array();
            if (hidden && !inputs[i].empty()) {
                const std::vector<float> h = pb::encode(session, *model, bytes[i]);
                w.key("hidden").begin_array();
                for (size_t t = 0; t < inputs[i].size(); ++t)
                    write_floats(w, h.data() + t * model->d_model, static_cast<size_t>(model->d_model));
                w.end_array();
            }
            w.end_object();
        }
        w.end_array().end_object();
        std::fwrite(w.str().data(), 1, w.str().size(), stdout);
        std::fputc('\n', stdout);
    } catch (const pb::Failure& f) {
        die(std::string(pb_status_name(f.status())) + ": " + f.what());
    } catch (const std::exception& e) {
        die(e.what());
    }
    return 0;
}
