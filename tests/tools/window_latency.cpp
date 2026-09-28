// window_latency: how the time of ONE window and the throughput of a batch change with the threads, for a model file.
//
//   window_latency MODEL [--size=512] [--iterations=30] [--threads=1,2,4,8] [--batch=48] [--kernel=auto]
//
// Latency: one input of --size bytes (one window), a session whose threads all work on that window
// (intra_threads = threads); the median and the best of --iterations runs. Throughput: --batch windows with teams of
// one thread (intra_threads = 1) and with the automatic split (intra_threads = 0). Every run of one configuration must
// give the same digests (checked); ratios are against one thread. The machine's load changes the absolute numbers far
// more than the ratios: report both with the conditions.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/failure.h"
#include "model/model.h"
#include "runtime/scan.h"
#include "runtime/session.h"

namespace {

[[noreturn]] void die(const std::string& message) {
    std::fprintf(stderr, "window_latency: %s\n", message.c_str());
    std::exit(2);
}

std::vector<int> int_list(const std::string& text) {
    std::vector<int> out;
    for (size_t at = 0; at < text.size();) {
        const size_t comma = std::min(text.find(',', at), text.size());
        out.push_back(std::atoi(text.substr(at, comma - at).c_str()));
        at = comma + 1;
    }
    return out;
}

std::vector<uint8_t> sample(size_t n, uint32_t seed) {
    // Mostly text with some binary bytes: the time of a window does not depend on its content, only its results do.
    std::vector<uint8_t> out(n);
    const std::string text = "level = 42; mode = fast\n# The quick brown fox jumps over 13 lazy dogs.\n";
    for (size_t i = 0; i < n; ++i) {
        seed = seed * 1664525u + 1013904223u;
        out[i] = (seed >> 28) < 12 ? static_cast<uint8_t>(text[(i + (seed >> 20)) % text.size()])
                                   : static_cast<uint8_t>(seed >> 24);
    }
    return out;
}

struct Timing {
    double median_ms = 0, best_ms = 0;
    uint64_t digest = 0;
};

Timing measure(pb::Session& session, const pb::Model& model, const std::vector<pb::Bytes>& inputs, int iterations,
               const pb::ScanOptions& o) {
    std::vector<double> ms;
    uint64_t first = 0;
    for (int i = 0; i < iterations + 2; ++i) {  // two warm-up runs
        const auto t0 = std::chrono::steady_clock::now();
        const pb::ScanResult r = pb::scan(session, model, inputs, o);
        const auto t1 = std::chrono::steady_clock::now();
        uint64_t digest = 1469598103934665603ull;
        for (const auto& input : r.inputs)
            for (const pb::WindowResult& w : input) digest = (digest ^ w.digest) * 1099511628211ull;
        if (i == 0) first = digest;
        if (digest != first) die("two runs of one configuration gave different results");
        if (i >= 2) ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    std::sort(ms.begin(), ms.end());
    return {ms[ms.size() / 2], ms.front(), first};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) die("usage: window_latency MODEL [--size=512] [--iterations=30] [--threads=1,2,4,8] [--batch=48]");
    int size = 512, iterations = 30, batch = 48;
    std::vector<int> threads = {1, 2, 4, 8};
    std::string kernel = "auto";
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        const size_t eq = a.find('=');
        const std::string key = a.substr(0, eq), value = eq == std::string::npos ? "" : a.substr(eq + 1);
        if (key == "--size")
            size = std::atoi(value.c_str());
        else if (key == "--iterations")
            iterations = std::max(1, std::atoi(value.c_str()));
        else if (key == "--threads")
            threads = int_list(value);
        else if (key == "--batch")
            batch = std::atoi(value.c_str());
        else if (key == "--kernel")
            kernel = value;
        else
            die("unknown option " + a);
    }
    try {
        const auto model = pb::load_model(argv[1]);
        pb::ScanOptions o;
        o.digest = true;
        o.whole = true;
        const std::vector<uint8_t> one = sample(static_cast<size_t>(size), 1);
        std::printf("model %s: d_model %d, %zu blocks; one window of %d bytes; %d iterations\n", argv[1],
                    model->d_model, model->blocks.size(), size, iterations);
        std::printf("%-8s %12s %10s %8s   %s\n", "threads", "median ms", "best ms", "speedup", "digest");
        double base = 0;
        uint64_t reference = 0;
        for (int k : threads) {
            pb::Session session(k, k, kernel);
            const Timing t = measure(session, *model, {{one.data(), one.size()}}, iterations, o);
            if (base == 0) base = t.median_ms, reference = t.digest;
            std::printf("%-8d %12.3f %10.3f %7.2fx   %016llx%s\n", k, t.median_ms, t.best_ms, base / t.median_ms,
                        static_cast<unsigned long long>(t.digest), t.digest == reference ? "" : "  DIFFERENT");
        }
        if (batch > 0) {
            std::vector<std::vector<uint8_t>> data;
            std::vector<pb::Bytes> inputs;
            for (int i = 0; i < batch; ++i) data.push_back(sample(static_cast<size_t>(size), 7 + i));
            for (const auto& d : data) inputs.push_back({d.data(), d.size()});
            const int runs = std::max(3, iterations / 10);
            std::printf("\nbatch of %d windows, %d iterations: windows per second\n", batch, runs);
            std::printf("%-8s %14s %14s\n", "threads", "teams of 1", "automatic");
            for (int k : threads) {
                pb::Session single(k, 1, kernel), automatic(k, 0, kernel);
                const Timing a = measure(single, *model, inputs, runs, o),
                             b = measure(automatic, *model, inputs, runs, o);
                std::printf("%-8d %14.1f %14.1f%s\n", k, batch * 1000.0 / a.median_ms, batch * 1000.0 / b.median_ms,
                            a.digest == b.digest ? "" : "  DIFFERENT");
            }
        }
    } catch (const pb::Failure& f) {
        die(f.what());
    }
    return 0;
}
