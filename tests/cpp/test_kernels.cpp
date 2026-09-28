// Every SIMD kernel against the scalar specification, bit for bit, on random data with awkward sizes (lengths that
// are not multiples of the vector width, partial channel blocks, carried state, taps before the first row).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/cpu.h"
#include "kernels/canonical.h"
#include "kernels/kernels.h"
#include "test.h"

#if (defined(__x86_64__) || defined(_M_X64))
#include <immintrin.h>
#endif

namespace {

using pb::kernels::Kernels;

struct Random {
    uint64_t s;
    explicit Random(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    float uniform(float lo, float hi) {
        return lo + (hi - lo) * static_cast<float>((next() >> 40) * (1.0 / (1ull << 24)));
    }
    // Mostly ordinary values, sometimes zeros of both signs and extremes, to exercise every rounding path.
    float value() {
        switch (next() % 16) {
            case 0: return 0.f;
            case 1: return -0.f;
            case 2: return uniform(-1e-30f, 1e-30f);
            case 3: return uniform(-50.f, 50.f);
            default: return uniform(-2.f, 2.f);
        }
    }
    std::vector<float> values(size_t n) {
        std::vector<float> v(n);
        for (float& x : v) x = value();
        return v;
    }
};

bool same_bits(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

// The SIMD kernels to test: those this CPU runs (all compiled-in tables when emulating another ISA).
std::vector<const Kernels*> simd_kernels() {
    std::vector<const Kernels*> out;
#if defined(PUREBYTE_TEST_ALL_KERNELS)
    if (pb::kernels::avx2_kernels()) out.push_back(pb::kernels::avx2_kernels());
    if (pb::kernels::neon_kernels()) out.push_back(pb::kernels::neon_kernels());
#else
    for (const char* name : {"avx2", "neon"})
        if (const Kernels* k = pb::kernels::find_kernels(name)) out.push_back(k);
#endif
    return out;
}

const Kernels& scalar() { return pb::kernels::scalar_kernels(); }

}  // namespace

TEST(kernels_list) {
    std::string names;
    for (const Kernels* k : simd_kernels()) names += std::string(names.empty() ? "" : ", ") + k->name;
    std::printf("     SIMD kernels compared with the scalar kernel: %s\n", names.empty() ? "none" : names.c_str());
    CHECK(pb::kernels::find_kernels("auto") != nullptr);
    CHECK(pb::kernels::find_kernels("scalar") == &scalar());
    CHECK(pb::kernels::find_kernels("unknown") == nullptr);
}

TEST(kernels_gemm_matches_scalar) {
    Random rng(1);
    for (const Kernels* k : simd_kernels())
        for (int ternary = 0; ternary < 2; ++ternary)
            for (int in : {8, 24, 37, 256})
                for (int out : {5, 8, 19})
                    for (int T : {1, 2, 5}) {
                        const int group = ternary ? (in % 8 == 0 ? 8 : in) : 0;
                        std::vector<int8_t> codes(static_cast<size_t>(out) * in);
                        for (int8_t& c : codes) c = static_cast<int8_t>(static_cast<int>(rng.next() % 3) - 1);
                        const std::vector<float> scales =
                            rng.values(static_cast<size_t>(out) * (ternary ? in / group : 1));
                        const std::vector<float> weights = rng.values(static_cast<size_t>(out) * in);
                        pb::kernels::Matrix W;
                        W.out = out;
                        W.in = in;
                        if (ternary) {
                            W.group = group;
                            W.codes = codes.data();
                            W.scales = scales.data();
                        } else {
                            W.weights = weights.data();
                        }
                        const size_t ldx = static_cast<size_t>(in) + 3, ldy = static_cast<size_t>(out) + 2;
                        const std::vector<float> X = rng.values(ldx * T);
                        std::vector<float> fold(8 * static_cast<size_t>(in));
                        std::vector<float> want(ldy * T, 7.f), got(ldy * T, 7.f);
                        const int o0 = out > 8 ? 3 : 0;
                        scalar().gemm(W, X.data(), ldx, T, want.data(), ldy, o0, out, fold.data());
                        k->gemm(W, X.data(), ldx, T, got.data(), ldy, o0, out, fold.data());
                        CHECK_MSG(same_bits(want, got),
                                  std::string(k->name) + " in=" + std::to_string(in) + " out=" + std::to_string(out));
                    }
}

TEST(kernels_dot_rows_matches_scalar) {
    Random rng(2);
    for (const Kernels* k : simd_kernels())
        for (int n : {1, 8, 13, 64, 256})
            for (int rows : {1, 8, 11})
                for (size_t pad : {size_t(0), size_t(5)}) {
                    const size_t lda = static_cast<size_t>(n) + pad;
                    const std::vector<float> A = rng.values(lda * rows), x = rng.values(static_cast<size_t>(n));
                    std::vector<float> want(static_cast<size_t>(rows)), got(static_cast<size_t>(rows));
                    scalar().dot_rows(A.data(), lda, rows, x.data(), n, want.data());
                    k->dot_rows(A.data(), lda, rows, x.data(), n, got.data());
                    CHECK_MSG(same_bits(want, got), std::string(k->name) + " n=" + std::to_string(n));
                }
}

TEST(kernels_norms_match_scalar) {
    Random rng(3);
    for (const Kernels* k : simd_kernels())
        for (int n : {4, 7, 256})
            for (int rows : {1, 16, 21}) {
                const size_t ld = static_cast<size_t>(n) + 1;
                const std::vector<float> X = rng.values(ld * rows), w = rng.values(static_cast<size_t>(n));
                std::vector<float> want(static_cast<size_t>(rows)), got(static_cast<size_t>(rows));
                scalar().rms_inv(X.data(), ld, rows, n, want.data());
                k->rms_inv(X.data(), ld, rows, n, got.data());
                CHECK_MSG(same_bits(want, got), std::string(k->name) + " rms n=" + std::to_string(n));
                std::vector<float> y1(static_cast<size_t>(n)), y2(static_cast<size_t>(n));
                scalar().scale_mul(X.data(), want[0], w.data(), y1.data(), n);
                k->scale_mul(X.data(), want[0], w.data(), y2.data(), n);
                CHECK_MSG(same_bits(y1, y2), std::string(k->name) + " scale_mul");
            }
}

TEST(kernels_conv_and_silu_match_scalar) {
    Random rng(4);
    for (const Kernels* k : simd_kernels())
        for (int channels : {3, 8, 20})
            for (int K : {1, 4})
                for (int history : {0, 3}) {
                    const int T = 9;
                    const size_t lds = static_cast<size_t>(channels) + 2;
                    const std::vector<float> src = rng.values(lds * (T + history)),
                                             kT = rng.values(static_cast<size_t>(K) * channels);
                    const std::vector<float> bias = rng.values(static_cast<size_t>(channels));
                    std::vector<float> want(static_cast<size_t>(T) * channels), got(want.size());
                    const float* row0 = src.data() + history * lds;
                    scalar().conv_silu(row0, lds, history, want.data(), channels, kT.data(), bias.data(), 0, T,
                                       channels, K);
                    k->conv_silu(row0, lds, history, got.data(), channels, kT.data(), bias.data(), 0, T, channels, K);
                    CHECK_MSG(same_bits(want, got), std::string(k->name) + " conv c=" + std::to_string(channels));
                }
    for (const Kernels* k : simd_kernels())
        for (int n : {1, 7, 8, 21}) {
            const std::vector<float> x = rng.values(static_cast<size_t>(n)), z = rng.values(static_cast<size_t>(n));
            std::vector<float> a(static_cast<size_t>(n)), b(static_cast<size_t>(n));
            scalar().silu(x.data(), a.data(), n);
            k->silu(x.data(), b.data(), n);
            CHECK_MSG(same_bits(a, b), std::string(k->name) + " silu");
            a = x;
            b = x;
            scalar().mul_silu(a.data(), z.data(), n);
            k->mul_silu(b.data(), z.data(), n);
            CHECK_MSG(same_bits(a, b), std::string(k->name) + " mul_silu");
        }
}

TEST(kernels_scan_matches_scalar) {
    Random rng(5);
    for (const Kernels* k : simd_kernels())
        for (int channels : {3, 8})
            for (int N : {5, 8, 16, 21})
                for (int include_D = 0; include_D < 2; ++include_D) {
                    const int T = 7;
                    const size_t xs = 11, bcs = static_cast<size_t>(2 * N) + 1, ts = 3;
                    const std::vector<float> x = rng.values(xs * T), bc = rng.values(bcs * T);
                    std::vector<float> dt(ts * T), dA(ts * T);
                    for (int t = 0; t < T; ++t) {
                        dt[t * ts] = rng.uniform(0.001f, 0.5f);
                        dA[t * ts] = rng.uniform(0.5f, 1.f);
                    }
                    const std::vector<float> state0 = rng.values(static_cast<size_t>(N) * 8);
                    std::vector<float> s1 = state0, s2 = state0, y1(xs * T, 3.f), y2(xs * T, 3.f);
                    pb::kernels::ScanBlock b;
                    b.x = x.data();
                    b.x_stride = xs;
                    b.B = bc.data();
                    b.C = bc.data() + N;
                    b.bc_stride = bcs;
                    b.dA = dA.data();
                    b.dt = dt.data();
                    b.t_stride = ts;
                    b.y_stride = xs;
                    b.D = 0.75f;
                    b.include_D = include_D != 0;
                    b.T = T;
                    b.channels = channels;
                    b.N = N;
                    b.state = s1.data();
                    b.y = y1.data();
                    scalar().scan(b);
                    b.state = s2.data();
                    b.y = y2.data();
                    k->scan(b);
                    CHECK_MSG(same_bits(y1, y2) && same_bits(s1, s2),
                              std::string(k->name) + " scan c=" + std::to_string(channels) + " N=" + std::to_string(N));
                }
}

#if (defined(__x86_64__) || defined(_M_X64))
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("fma")))
#endif
static float
hardware_fma(float a, float b, float c) {
    return _mm_cvtss_f32(_mm_fmadd_ss(_mm_set_ss(a), _mm_set_ss(b), _mm_set_ss(c)));
}

TEST(canonical_fma_is_correctly_rounded) {
    if (!pb::cpu_features().avx2_fma) return;
    Random rng(6);
    int mismatches = 0;
    for (int i = 0; i < 2000000; ++i) {
        // exponents over the whole float range, and products that cancel against c
        const int ea = static_cast<int>(rng.next() % 60) - 30, eb = static_cast<int>(rng.next() % 60) - 30;
        const float a = std::ldexp(rng.uniform(-1.f, 1.f), ea), b = std::ldexp(rng.uniform(-1.f, 1.f), eb);
        const float c = (i % 3 == 0) ? -a * b + std::ldexp(rng.uniform(-1.f, 1.f), ea + eb - 20)
                                     : std::ldexp(rng.uniform(-1.f, 1.f), ea + eb);
        const float want = hardware_fma(a, b, c), got = pb::kernels::canonical::fma_rn(a, b, c);
        mismatches += std::memcmp(&want, &got, sizeof want) != 0;
    }
    CHECK_MSG(mismatches == 0, std::to_string(mismatches) + " differences with the hardware FMA");
}
#endif
