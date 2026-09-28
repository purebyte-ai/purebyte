// The hot loops of the runtime behind one table of function pointers, so that the rest of the code is the same on
// every CPU. Three implementations: `scalar` (portable, and the executable SPECIFICATION of the arithmetic), `avx2`
// (x86-64 with AVX2 + FMA) and `neon` (AArch64). All three perform the same IEEE operations in the same order and
// therefore return the same bits on one platform (spec/FORMAT.md, "Numerics"):
//   * a reduction of n products uses 8 accumulator lanes (element i goes to lane i % 8) updated with fused
//     multiply-adds over the first n - n % 8 elements, combines the lanes as ((l0+l4)+(l1+l5))+((l2+l6)+(l3+l7)), then
//     adds the remaining elements one by one with a separate multiply and add;
//   * SiLU inside the blocks uses the polynomial exp of `canonical.h`, never the C library's.
//
// This header is plain C++ with POD arguments on purpose: the SIMD translation units are compiled with special
// instruction-set flags and must not instantiate any inline function or template that other units also use.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/numerics.h"  // no contraction of a * b + c, on every compiler

namespace pb::kernels {

// y = W x for a matrix W of shape [out, in]: ternary (codes in {-1, 0, +1}, one scale per `group` inputs of a row) or
// float (row-major weights).
struct Matrix {
    int out = 0;
    int in = 0;
    int group = 0;                   // ternary only; in % group == 0
    const int8_t* codes = nullptr;   // ternary [out * in]
    const float* scales = nullptr;   // ternary [out * (in / group)]
    const float* weights = nullptr;  // float [out * in]
};

// One block of up to 8 channels of one SSM head, scanned over T positions. For channel c (lane c) and position t:
//   dx = dt[t] * x[t][c];  s[n] = dA[t] * s[n] + dx * B[t][n];  y[t][c] = sum_n s[n] * C[t][n] + D * x[t][c]
// with the reduction over n done in the canonical order (see above) and `state` carried across positions.
struct ScanBlock {
    float* state = nullptr;  // [N][8]: n-major, lane = channel; carried in and out
    const float* x = nullptr;
    size_t x_stride = 0;  // x[t * x_stride + c]
    const float* B = nullptr;
    const float* C = nullptr;
    size_t bc_stride = 0;  // B[t * bc_stride + n]
    const float* dA = nullptr;
    const float* dt = nullptr;
    size_t t_stride = 0;  // dA[t * t_stride], dt[t * t_stride]
    float* y = nullptr;
    size_t y_stride = 0;  // y[t * y_stride + c]
    float D = 0.f;
    bool include_D = true;
    int T = 0;
    int channels = 0;  // 1..8
    int N = 0;         // state size
};

struct Kernels {
    const char* name;

    // Y[t * ldy + o] = dot(row o of W, X + t * ldx) for o in [o0, o1) and t in [0, T). A ternary row is first folded
    // to floats (code * scale, exact). `fold` is scratch for 8 * W.in floats.
    void (*gemm)(const Matrix& W, const float* X, size_t ldx, int T, float* Y, size_t ldy, int o0, int o1, float* fold);
    // y[r] = dot(A + r * lda, x, n) for r in [0, rows).
    void (*dot_rows)(const float* A, size_t lda, int rows, const float* x, int n, float* y);
    // inv[r] = 1 / sqrt(mean(x^2) + 1e-5) of row r, the sum of squares accumulated in double in element order.
    void (*rms_inv)(const float* X, size_t ldx, int rows, int n, float* inv);
    // y[i] = x[i] * s * w[i] (two roundings, in that order). y may alias x.
    void (*scale_mul)(const float* x, float s, const float* w, float* y, int n);
    // Causal depthwise convolution + SiLU for positions [t0, t1) of `channels` channels with K taps:
    //   dst[t][c] = silu(bias[c] + sum_k kT[k][c] * src[t - (K - 1) + k][c])   (fused multiply-adds, k ascending)
    // Taps before row -history are absent (not zero: skipped). kT is [K][channels].
    void (*conv_silu)(const float* src, size_t lds, int history, float* dst, size_t ldd, const float* kT,
                      const float* bias, int t0, int t1, int channels, int K);
    // y[i] = silu(x[i]); may alias.
    void (*silu)(const float* x, float* y, int n);
    // y[i] *= silu(z[i]).
    void (*mul_silu)(float* y, const float* z, int n);
    void (*scan)(const ScanBlock& block);
};

const Kernels& scalar_kernels();
const Kernels* avx2_kernels();  // nullptr when not built for this architecture
const Kernels* neon_kernels();  // nullptr when not built for this architecture

// The kernels to use for `name` ("auto", "scalar", "avx2", "neon"). nullptr when unknown or when this CPU cannot run
// them; `auto` always succeeds.
const Kernels* find_kernels(const char* name);

}  // namespace pb::kernels
