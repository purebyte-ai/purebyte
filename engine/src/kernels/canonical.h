// The scalar building blocks of the numerics contract. They define, operation by operation, what every kernel must
// compute; the SIMD kernels reproduce them lane by lane. Never include this header from a translation unit compiled
// with instruction-set flags (see kernels.h).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#include "core/numerics.h"  // no contraction of a * b + c, on every compiler

namespace pb::kernels::canonical {

inline uint64_t bits_of(double x) {
    uint64_t b;
    std::memcpy(&b, &x, sizeof b);
    return b;
}
inline double double_of(uint64_t b) {
    double x;
    std::memcpy(&x, &b, sizeof x);
    return x;
}

// Correctly rounded fused multiply-add, a * b + c with a single rounding (IEEE 754 fusedMultiplyAdd).
// Where the compiler targets a hardware FMA it is one instruction. Elsewhere it is emulated: a * b is exact in double
// (24 + 24 significant bits < 53); rounding the double sum to ODD and then to float gives the correctly rounded result
// (Boldo & Melquiond, "Emulation of FMA and correctly rounded sums: proved algorithms using rounding to odd", 2008).
// The C library's fmaf is not used: some implementations round twice.
inline float fma_rn(float a, float b, float c) {
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__FMA__) || defined(__aarch64__))
    return __builtin_fmaf(a, b, c);
#else
    const double p = static_cast<double>(a) * static_cast<double>(b);
    const double cd = static_cast<double>(c);
    double s = p + cd;
    if (!std::isfinite(s)) return static_cast<float>(s);
    const double z = s - p;
    const double err = (p - (s - z)) + (cd - z);  // exact error of the sum (Knuth's TwoSum)
    uint64_t b64 = bits_of(s);
    if (err != 0.0 && (b64 & 1u) == 0) {
        // The exact sum lies strictly between s and its neighbour on the side of `err`, and exactly one of the two
        // has an odd significand: step to it.
        const bool away_from_zero = (err > 0.0) == (s > 0.0);
        b64 = away_from_zero ? b64 + 1u : b64 - 1u;
        s = double_of(b64);
    }
    return static_cast<float>(s);
#endif
}

// ((l0 + l4) + (l1 + l5)) + ((l2 + l6) + (l3 + l7)): the reduction tree of the 8 accumulator lanes.
inline float hsum8(const float l[8]) {
    const float q0 = l[0] + l[4], q1 = l[1] + l[5], q2 = l[2] + l[6], q3 = l[3] + l[7];
    return (q0 + q1) + (q2 + q3);
}

// The canonical dot product (see kernels.h).
inline float dot(const float* a, const float* x, int n) {
    float lanes[8] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
    const int n8 = n & ~7;
    for (int i = 0; i < n8; i += 8)
        for (int l = 0; l < 8; ++l) lanes[l] = fma_rn(a[i + l], x[i + l], lanes[l]);
    float r = hsum8(lanes);
    for (int i = n8; i < n; ++i) r = r + a[i] * x[i];
    return r;
}

// Comparisons with the operand order of the x86 MAXPS / MINPS instructions (the second operand wins on ties and NaN).
inline float max_ps(float a, float b) { return a > b ? a : b; }
inline float min_ps(float a, float b) { return a < b ? a : b; }

// exp(x) as the Cephes polynomial (as in avx_mathfun): relative error ~1e-7. Used by SiLU inside the blocks, where
// it runs millions of times per window and the C library's expf would dominate the time.
inline float exp_poly(float x) {
    x = min_ps(max_ps(x, -88.3762626647949f), 88.3762626647949f);
    float fx = std::floor(fma_rn(x, 1.44269504088896341f, 0.5f));
    x = fma_rn(-fx, 0.693359375f, x);
    x = fma_rn(-fx, -2.12194440e-4f, x);
    const float z = x * x;
    float y = 1.9875691500E-4f;
    y = fma_rn(y, x, 1.3981999507E-3f);
    y = fma_rn(y, x, 8.3334519073E-3f);
    y = fma_rn(y, x, 4.1665795894E-2f);
    y = fma_rn(y, x, 1.6666665459E-1f);
    y = fma_rn(y, x, 5.0000001201E-1f);
    y = fma_rn(y, z, x + 1.0f);
    const uint32_t exponent = static_cast<uint32_t>(static_cast<int32_t>(fx) + 0x7f) << 23;
    float scale;
    std::memcpy(&scale, &exponent, sizeof scale);
    return y * scale;
}

// SiLU inside the blocks: x / (1 + exp(0 - x)) with the polynomial exp.
inline float silu_poly(float x) { return x / (1.0f + exp_poly(0.0f - x)); }

}  // namespace pb::kernels::canonical
