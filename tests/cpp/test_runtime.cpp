// Unit tests of the runtime's building blocks: team shares and barriers, the thread pool (a worker that cannot start
// included), window enumeration, the query template and BIOES decoding (against an exhaustive search on small cases,
// and against the full-matrix search on random lattices).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "core/failure.h"
#include "core/threads.h"
#include "heads/bioes.h"
#include "model/model.h"
#include "runtime/windows.h"
#include "test.h"

TEST(team_share_covers_everything_once) {
    for (int size = 1; size <= 9; ++size)
        for (int n : {0, 1, 7, 16, 100, 513})
            for (int align : {1, 8, 16}) {
                std::vector<int> seen(static_cast<size_t>(n), 0);
                int previous_end = 0;
                for (int m = 0; m < size; ++m) {
                    const pb::Team team{m, size, nullptr};
                    const auto [a, b] = team.share(n, align);
                    CHECK(a == previous_end);  // contiguous and in member order
                    CHECK(a <= b && b <= n);
                    if (b < n) CHECK(b % align == 0);
                    for (int i = a; i < b; ++i) ++seen[static_cast<size_t>(i)];
                    previous_end = b;
                }
                CHECK(previous_end == n);
                for (int s : seen) CHECK(s == 1);
            }
}

TEST(barrier_orders_the_stages_of_a_team) {
    const int members = 4, rounds = 2000;
    pb::Barrier barrier(members);
    std::atomic<int> counter{0};
    std::atomic<bool> wrong{false};
    std::vector<std::thread> threads;
    for (int m = 0; m < members; ++m)
        threads.emplace_back([&] {
            for (int r = 0; r < rounds; ++r) {
                counter.fetch_add(1);
                barrier.wait();
                if (counter.load() != members * (r + 1)) wrong = true;  // everyone has arrived
                barrier.wait();
            }
        });
    for (std::thread& t : threads) t.join();
    CHECK(!wrong);
}

TEST(thread_pool_runs_every_index_and_rethrows) {
    pb::ThreadPool pool(5);
    std::vector<std::atomic<int>> hits(5);
    for (int round = 0; round < 50; ++round) pool.run([&](int i) { hits[static_cast<size_t>(i)].fetch_add(1); });
    for (auto& h : hits) CHECK(h.load() == 50);
    bool thrown = false;
    try {
        pool.run([](int i) {
            if (i == 3) throw std::runtime_error("member 3");
        });
    } catch (const std::runtime_error&) {
        thrown = true;
    }
    CHECK(thrown);
    int after = 0;
    pool.run([&](int i) {
        if (i == 0) after = 1;
    });
    CHECK(after == 1);  // the pool still works after an exception
}

TEST(windows_follow_the_specification) {
    using pb::enumerate_windows;
    auto spans = [](int64_t size, int window, int stride, int min) {
        std::vector<std::pair<int64_t, int>> out;
        for (const pb::WindowSpan& w : enumerate_windows(size, window, stride, min)) out.push_back({w.start, w.length});
        return out;
    };
    using V = std::vector<std::pair<int64_t, int>>;
    CHECK(spans(0, 512, 384, 24) == V{});
    CHECK(spans(23, 512, 384, 24) == V{});
    CHECK(spans(24, 512, 384, 24) == (V{{0, 24}}));
    CHECK(spans(512, 512, 384, 24) == (V{{0, 512}}));
    CHECK(spans(513, 512, 384, 24) == (V{{0, 512}, {384, 129}}));
    CHECK(spans(1000, 512, 384, 24) == (V{{0, 512}, {384, 512}, {768, 232}}));
    CHECK(spans(790, 512, 384, 24) == (V{{0, 512}, {384, 406}}));
    CHECK(spans(100, 40, 25, 24) == (V{{0, 40}, {25, 40}, {50, 40}, {75, 25}}));
    // The next window would be 20 bytes, under the shortest: a last one aligned to the end covers bytes 90..94, which
    // no window saw before (review R1, finding 8).
    CHECK(spans(95, 40, 25, 24) == (V{{0, 40}, {25, 40}, {50, 40}, {55, 40}}));
    CHECK(spans(65, 64, 64, 24) == (V{{0, 64}, {1, 64}}));
    CHECK(spans(333, 64, 64, 24) == (V{{0, 64}, {64, 64}, {128, 64}, {192, 64}, {256, 64}, {269, 64}}));
    CHECK(pb::nominal_window_count(1000, 512, 384) == 3);
    CHECK(pb::nominal_window_count(10, 512, 384) == 1);
}

TEST(windows_cover_every_byte) {
    // Any geometry (stride <= window, min <= window) and input size: the windows cover every byte of an input of at
    // least `min` bytes, none is shorter than `min`, and there are as many as nominal_window_count says.
    pbtest::Random rng(17);
    for (int trial = 0; trial < 20000; ++trial) {
        const int window = 1 + rng.below(80), stride = 1 + rng.below(window), min = 1 + rng.below(window);
        const int64_t size = rng.below(400);
        const std::vector<pb::WindowSpan> w = pb::enumerate_windows(size, window, stride, min);
        if (size < min) {
            CHECK(w.empty());
            continue;
        }
        std::vector<char> seen(static_cast<size_t>(size), 0);
        for (const pb::WindowSpan& s : w) {
            CHECK(s.length >= min && s.length <= window && s.start >= 0 && s.start + s.length <= size);
            for (int64_t i = s.start; i < s.start + s.length; ++i) seen[static_cast<size_t>(i)] = 1;
        }
        CHECK(std::all_of(seen.begin(), seen.end(), [](char c) { return c == 1; }));
        CHECK(static_cast<int64_t>(w.size()) == pb::nominal_window_count(size, window, stride));
    }
}

TEST(windows_of_the_released_geometry_are_unchanged) {
    // The released models declare windows of 512 bytes every 384, and at least 24 (secrets) or 1 (pii): 512 - 384 =
    // 128 bytes always remain for the next window, so no walk ever stopped before the end, and the windows are those
    // of the walk before the last window aligned to the end existed.
    auto walk_without_last_window = [](int64_t size, int window, int stride, int min) {
        std::vector<pb::WindowSpan> out;
        for (int64_t start = 0;; start += stride) {
            const int64_t length = std::min<int64_t>(window, size - start);
            if (length < min) break;
            out.push_back({start, static_cast<int32_t>(length)});
            if (start + window >= size) break;
        }
        return out;
    };
    for (int min : {1, 24})
        for (int64_t size = 0; size <= 20000; ++size) {
            const std::vector<pb::WindowSpan> now = pb::enumerate_windows(size, 512, 384, min),
                                              before = walk_without_last_window(size, 512, 384, min);
            bool same = now.size() == before.size();
            for (size_t k = 0; same && k < now.size(); ++k)
                same = now[k].start == before[k].start && now[k].length == before[k].length;
            CHECK_MSG(same, "size " + std::to_string(size) + ", min " + std::to_string(min));
        }
}

TEST(query_template_encodes_and_refuses) {
    pb::QueryTemplate q;
    q.region = 16;
    q.max_queries = 2;
    const std::vector<uint8_t> bytes = q.encode({"ab", "c"});
    CHECK(std::string(bytes.begin(), bytes.end()) == std::string("?ab\n?c\n") + std::string(9, '\n'));
    CHECK_FAILS(q.encode({"a", "b", "c"}), PB_ERR_ARGUMENT);
    CHECK_FAILS(q.encode({"a much too long query"}), PB_ERR_ARGUMENT);
}

namespace {

bool valid_bioes(const std::vector<int>& path) {
    auto kind = [](int l) { return l == 0 ? -1 : (l - 1) % 4; };  // 0 B, 1 I, 2 E, 3 S
    auto entity = [](int l) { return (l - 1) / 4; };
    if (kind(path.front()) == 1 || kind(path.front()) == 2) return false;
    if (kind(path.back()) == 0 || kind(path.back()) == 1) return false;
    for (size_t t = 1; t < path.size(); ++t) {
        const int a = path[t - 1], b = path[t];
        const bool inside = kind(a) == 0 || kind(a) == 1;
        if (inside != (kind(b) == 1 || kind(b) == 2)) return false;
        if (inside && entity(a) != entity(b)) return false;
    }
    return true;
}

}  // namespace

TEST(viterbi_finds_the_best_valid_sequence) {
    pbtest::Random rng(7);
    for (int trial = 0; trial < 60; ++trial) {
        const int entities = 1 + trial % 2, L = pb::bioes::labels_for(entities), T = 1 + trial % 5;
        std::vector<float> logp(static_cast<size_t>(T) * L), bias(static_cast<size_t>(L), 0.f);
        for (float& v : logp) v = -static_cast<float>(rng.below(1000)) / 100.f;
        for (int k = 1; k < L; ++k) bias[static_cast<size_t>(k)] = static_cast<float>(rng.below(200) - 100) / 100.f;
        std::vector<int> path;
        pb::bioes::viterbi(logp.data(), T, L, entities, bias.data(), path);
        CHECK(valid_bioes(path));
        // exhaustive search over every sequence: none valid scores higher
        double best = -1e300;
        std::vector<int> seq(static_cast<size_t>(T), 0);
        for (long long code = 0, total = static_cast<long long>(std::pow(L, T)); code < total; ++code) {
            long long c = code;
            for (int t = 0; t < T; ++t, c /= L) seq[static_cast<size_t>(t)] = static_cast<int>(c % L);
            if (!valid_bioes(seq)) continue;
            double s = 0;
            for (int t = 0; t < T; ++t)
                s += static_cast<double>(logp[static_cast<size_t>(t) * L + seq[t]]) + bias[seq[t]];
            best = std::max(best, s);
        }
        double got = 0;
        for (int t = 0; t < T; ++t)
            got += static_cast<double>(logp[static_cast<size_t>(t) * L + path[t]]) + bias[path[t]];
        CHECK(std::fabs(got - best) < 1e-9);
    }
}

namespace {

// The Viterbi search as first written: every label against every label, O(T L^2) with an L x L transition matrix.
// The decoder must give its paths exactly, ties and non-finite scores included.
std::vector<int> viterbi_by_matrix(const float* logp, int T, int L, int entities, const float* bias) {
    constexpr double kForbidden = -1e30;
    auto can_start = [](int l) { return l == 0 || (l - 1) % 4 == 0 || (l - 1) % 4 == 3; };
    auto can_end = [](int l) { return l == 0 || (l - 1) % 4 == 2 || (l - 1) % 4 == 3; };
    std::vector<double> trans(static_cast<size_t>(L) * L, kForbidden);
    for (int src = 0; src < L; ++src)
        for (int dst = 0; dst < L; ++dst)
            if (can_end(src) && can_start(dst)) trans[static_cast<size_t>(src) * L + dst] = 0.0;
    for (int e = 0; e < entities; ++e) {
        const int B = 1 + 4 * e, I = 2 + 4 * e, E = 3 + 4 * e;
        trans[static_cast<size_t>(B) * L + I] = trans[static_cast<size_t>(B) * L + E] = 0.0;
        trans[static_cast<size_t>(I) * L + I] = trans[static_cast<size_t>(I) * L + E] = 0.0;
    }
    std::vector<double> dp(static_cast<size_t>(L)), next(static_cast<size_t>(L));
    std::vector<int> back(static_cast<size_t>(T) * L, 0);
    for (int k = 0; k < L; ++k)
        dp[k] = static_cast<double>(logp[k]) + (bias ? bias[k] : 0.0) + (can_start(k) ? 0.0 : kForbidden);
    for (int t = 1; t < T; ++t) {
        for (int k = 0; k < L; ++k) {
            double best = -1e300;
            int arg = 0;
            for (int j = 0; j < L; ++j) {
                const double v = dp[j] + trans[static_cast<size_t>(j) * L + k];
                if (v > best) {
                    best = v;
                    arg = j;
                }
            }
            back[static_cast<size_t>(t) * L + k] = arg;
            next[k] = best + static_cast<double>(logp[static_cast<size_t>(t) * L + k]) + (bias ? bias[k] : 0.0);
        }
        dp.swap(next);
    }
    int last = 0;
    double best = -1e300;
    for (int k = 0; k < L; ++k) {
        const double v = dp[k] + (can_end(k) ? 0.0 : kForbidden);
        if (v > best) {
            best = v;
            last = k;
        }
    }
    std::vector<int> path(static_cast<size_t>(T), 0);
    path[T - 1] = last;
    for (int t = T - 1; t > 0; --t) path[t - 1] = back[static_cast<size_t>(t) * L + path[t]];
    return path;
}

}  // namespace

TEST(viterbi_equals_the_full_matrix_search) {
    // The decoder costs O(L) per position (review R1, finding 6); its paths are those of the O(L^2) search on random
    // lattices, with many ties (scores from a few values), huge operating biases (so that forbidden steps compete),
    // -inf and NaN log-probabilities.
    pbtest::Random rng(29);
    const float kValues[] = {-0.5f, -1.f, -1.f, -2.f, -30.f, -INFINITY, NAN, 0.f, -1e30f, -3e38f};
    for (int trial = 0; trial < 3000; ++trial) {
        const int entities = 1 + rng.below(4), L = pb::bioes::labels_for(entities), T = 1 + rng.below(12);
        std::vector<float> logp(static_cast<size_t>(T) * L), bias(static_cast<size_t>(L), 0.f);
        const bool extreme = trial % 3 == 0;
        for (float& v : logp) v = extreme ? kValues[rng.below(10)] : -static_cast<float>(rng.below(8)) / 2.f;
        for (int k = 1; k < L; ++k)
            bias[static_cast<size_t>(k)] =
                trial % 5 == 0 ? (rng.below(2) ? 3e30f : -3e30f) : static_cast<float>(rng.below(5) - 2) / 2.f;
        std::vector<int> path;
        pb::bioes::viterbi(logp.data(), T, L, entities, bias.data(), path);
        CHECK_MSG(path == viterbi_by_matrix(logp.data(), T, L, entities, bias.data()),
                  "trial " + std::to_string(trial));
    }
}

TEST(thread_pool_that_cannot_start_a_worker_fails_cleanly) {
    // A worker that cannot be created (a thread or memory limit of the system) makes the pool stop and join the
    // workers already started, and report PB_ERR_NO_MEMORY, instead of terminating the process (review R1, finding 4).
    for (int refused : {1, 3, 7}) {
        static int refuse;
        refuse = refused;
        CHECK_FAILS(pb::ThreadPool(8, [](int i) { return i != refuse; }), PB_ERR_NO_MEMORY);
    }
    pb::ThreadPool pool(4, [](int) { return true; });  // the pools after it work
    std::atomic<int> hits{0};
    pool.run([&](int) { hits.fetch_add(1); });
    CHECK(hits.load() == 4);
}

TEST(spans_come_from_the_decoded_labels) {
    // O B I E S O B(dangling) : labels of entity 0 are 1..4; entity 1 is 5..8
    const std::vector<int> path = {0, 1, 2, 3, 8, 0, 5, 6};
    std::vector<float> logp(path.size() * 9, std::log(0.5f));
    std::vector<pb::Span> spans;
    pb::bioes::spans(path, logp.data(), 9, spans);
    CHECK(spans.size() == 2);
    if (spans.size() == 2) {
        CHECK(spans[0].start == 1 && spans[0].end == 4 && spans[0].type == 0);
        CHECK(spans[1].start == 4 && spans[1].end == 5 && spans[1].type == 1);
        CHECK(std::fabs(spans[0].confidence - 0.5f) < 1e-6f);
    }
}
