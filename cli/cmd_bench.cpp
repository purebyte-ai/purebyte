// purebyte bench: latency of small decisions and throughput on this machine, with the kernel and threads used.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "commands.h"
#include "core/json.h"
#include "setup.h"

namespace cli {

namespace {

using Clock = std::chrono::steady_clock;

// Deterministic text that looks like source code (identifiers, spaces, punctuation, newlines).
std::vector<uint8_t> sample(size_t n, uint64_t seed) {
    static const char kAlphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_ =(){}[];:,.\"'\n    ";
    std::vector<uint8_t> out(n);
    uint64_t s = seed * 0x9E3779B97F4A7C15ull + 1;
    for (uint8_t& c : out) {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        c = static_cast<uint8_t>(kAlphabet[s % (sizeof kAlphabet - 1)]);
    }
    return out;
}

double scan_ms(pb_session* session, const pb_model* model, const std::vector<uint8_t>& input) {
    pb_input in{input.data(), input.size()};
    pb_scan_result* result = nullptr;
    pb_error err;
    const auto t0 = Clock::now();
    check(pb_scan(session, model, &in, 1, nullptr, &result, &err), err, "benchmark");
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    pb_scan_result_free(result);
    return ms;
}

double percentile(std::vector<double> v, double p) {
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())))];
}

}  // namespace

int command_bench(const std::vector<std::string>& argv) {
    std::vector<OptionSpec> spec = model_options();
    spec.insert(spec.end(),
                {
                    {"sizes", "B1,B2,...", "input sizes of the latency test in bytes (default 64,256,1024,4096)"},
                    {"repeats", "N", "measurements per size (default 30)"},
                    {"throughput-kb", "N", "size of the throughput test in KB (default 256)"},
                    {"json", nullptr, "machine-readable output"},
                    {"help", nullptr, "show this help"},
                });
    const Args a(argv, 2, spec);
    if (a.has("help")) {
        std::fputs(
            ("usage: purebyte bench --model NAME [options]\n\nLatency of small decisions (median and 95th percentile) and "
             "throughput, on this machine.\n\n" +
             options_help(spec))
                .c_str(),
            stdout);
        return 0;
    }
    const Setup s = setup(a, "secrets-code");
    const pb_model* model = s.specialist.members[0].handle.get();
    const int repeats = a.integer("repeats", 30, 1, 100000);
    std::vector<size_t> sizes;
    for (const std::string& item : split_list(a.text("sizes", "64,256,1024,4096"))) {
        char* end = nullptr;
        const unsigned long long size = std::strtoull(item.c_str(), &end, 10);
        if (*end || size == 0 || size > (64ull << 20))
            throw UsageError("--sizes takes byte counts from 1 to 67108864, not `" + item + "`");
        sizes.push_back(static_cast<size_t>(size));
    }
    if (sizes.empty()) throw UsageError("--sizes needs at least one size");
    pb::json::Writer w;
    w.begin_object()
        .field("purebyte", pb_version())
        .field("model", s.specialist.name)
        .field("kernel", pb_session_kernel(s.session.get()));
    w.field("threads", s.threads).key("latency").begin_array();
    if (!a.has("json"))
        std::printf("model %s, kernel %s, %d thread(s)\n\n%10s %10s %10s\n", s.specialist.name.c_str(),
                    pb_session_kernel(s.session.get()), s.threads, "bytes", "p50 ms", "p95 ms");
    for (size_t size : sizes) {
        const std::vector<uint8_t> input = sample(size, size);
        for (int i = 0; i < 3; ++i) scan_ms(s.session.get(), model, input);  // warm-up
        std::vector<double> ms;
        for (int i = 0; i < repeats; ++i) ms.push_back(scan_ms(s.session.get(), model, input));
        const double p50 = percentile(ms, 0.5), p95 = percentile(ms, 0.95);
        w.begin_object()
            .field("bytes", static_cast<int64_t>(size))
            .field("p50_ms", p50)
            .field("p95_ms", p95)
            .end_object();
        if (!a.has("json")) std::printf("%10zu %10.2f %10.2f\n", size, p50, p95);
    }
    w.end_array();
    const size_t kb = static_cast<size_t>(a.integer("throughput-kb", 256, 1, 1 << 20));
    const std::vector<uint8_t> big = sample(kb * 1024, 7);
    const double ms = scan_ms(s.session.get(), model, big);
    const double kb_per_s = static_cast<double>(kb) / (ms / 1000.0);
    w.key("throughput")
        .begin_object()
        .field("kilobytes", static_cast<int64_t>(kb))
        .field("kb_per_s", kb_per_s)
        .end_object()
        .end_object();
    if (a.has("json"))
        std::printf("%s\n", w.str().c_str());
    else
        std::printf("\nthroughput on %zu KB: %.1f KB/s\n", kb, kb_per_s);
    return 0;
}

}  // namespace cli
