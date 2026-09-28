// Portable kernels: the executable specification of the numerics (see kernels.h). Readable first; they are the
// fallback on CPUs without a SIMD kernel and the oracle the SIMD kernels are tested against, bit for bit.
#include <cmath>
#include <cstddef>

#include "kernels/canonical.h"
#include "kernels/kernels.h"

namespace pb::kernels {

namespace {

using canonical::dot;
using canonical::fma_rn;
using canonical::hsum8;
using canonical::silu_poly;

void fold_row(const Matrix& W, int o, float* row) {
    const int groups = W.in / W.group;
    const int8_t* codes = W.codes + static_cast<size_t>(o) * W.in;
    const float* scales = W.scales + static_cast<size_t>(o) * groups;
    for (int g = 0; g < groups; ++g)
        for (int i = g * W.group; i < (g + 1) * W.group; ++i) row[i] = static_cast<float>(codes[i]) * scales[g];
}

void gemm(const Matrix& W, const float* X, size_t ldx, int T, float* Y, size_t ldy, int o0, int o1, float* fold) {
    for (int o = o0; o < o1; ++o) {
        const float* row = W.weights ? W.weights + static_cast<size_t>(o) * W.in : fold;
        if (!W.weights) fold_row(W, o, fold);
        for (int t = 0; t < T; ++t) Y[t * ldy + o] = dot(row, X + t * ldx, W.in);
    }
}

void dot_rows(const float* A, size_t lda, int rows, const float* x, int n, float* y) {
    for (int r = 0; r < rows; ++r) y[r] = dot(A + r * lda, x, n);
}

void rms_inv(const float* X, size_t ldx, int rows, int n, float* inv) {
    for (int r = 0; r < rows; ++r) {
        const float* x = X + r * ldx;
        double sum = 0.0;
        for (int i = 0; i < n; ++i) sum += static_cast<double>(x[i]) * static_cast<double>(x[i]);
        inv[r] = 1.0f / std::sqrt(static_cast<float>(sum / n) + 1e-5f);
    }
}

void scale_mul(const float* x, float s, const float* w, float* y, int n) {
    for (int i = 0; i < n; ++i) y[i] = x[i] * s * w[i];
}

void conv_silu(const float* src, size_t lds, int history, float* dst, size_t ldd, const float* kT, const float* bias,
               int t0, int t1, int channels, int K) {
    for (int t = t0; t < t1; ++t)
        for (int c = 0; c < channels; ++c) {
            float a = bias[c];
            for (int k = 0; k < K; ++k) {
                const ptrdiff_t tt = static_cast<ptrdiff_t>(t) - (K - 1) + k;
                if (tt >= -history)
                    a = fma_rn(kT[static_cast<size_t>(k) * channels + c], src[tt * static_cast<ptrdiff_t>(lds) + c], a);
            }
            dst[t * ldd + c] = silu_poly(a);
        }
}

void silu(const float* x, float* y, int n) {
    for (int i = 0; i < n; ++i) y[i] = silu_poly(x[i]);
}

void mul_silu(float* y, const float* z, int n) {
    for (int i = 0; i < n; ++i) y[i] = y[i] * (z[i] / (1.0f + canonical::exp_poly(0.0f - z[i])));
}

void scan(const ScanBlock& b) {
    const int N8 = b.N & ~7;
    for (int t = 0; t < b.T; ++t) {
        const float dA = b.dA[t * b.t_stride], dt = b.dt[t * b.t_stride];
        const float* B = b.B + t * b.bc_stride;
        const float* C = b.C + t * b.bc_stride;
        for (int c = 0; c < b.channels; ++c) {
            const float xv = b.x[t * b.x_stride + c];
            const float dx = dt * xv;
            float lanes[8] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
            for (int n = 0; n < N8; ++n) {
                float& s = b.state[n * 8 + c];
                s = fma_rn(dx, B[n], dA * s);
                lanes[n & 7] = fma_rn(s, C[n], lanes[n & 7]);
            }
            float o = hsum8(lanes);
            for (int n = N8; n < b.N; ++n) {
                float& s = b.state[n * 8 + c];
                s = dA * s + dx * B[n];
                o = o + s * C[n];
            }
            b.y[t * b.y_stride + c] = o + (b.include_D ? b.D * xv : 0.0f);
        }
    }
}

const Kernels kScalar = {"scalar", gemm, dot_rows, rms_inv, scale_mul, conv_silu, silu, mul_silu, scan};

}  // namespace

const Kernels& scalar_kernels() { return kScalar; }

}  // namespace pb::kernels
