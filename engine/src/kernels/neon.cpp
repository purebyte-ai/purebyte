// AArch64 kernels with Advanced SIMD (NEON). Same operations in the same order as the scalar specification
// (scalar.cpp): an 8-lane accumulator of the specification is a pair of 4-lane registers (lanes 0-3, lanes 4-7), and
// the reduction tree ((l0+l4)+(l1+l5))+((l2+l6)+(l3+l7)) is a lane-wise add of the pair followed by two pairwise adds.
//
// NEON is part of the AArch64 baseline, so this unit needs no special compiler flags and may use the scalar helpers
// of canonical.h for partial vectors.
//
// A universal macOS build compiles every source for every architecture: on the x86_64 slice this unit only says that
// there is no NEON table.
#if defined(__aarch64__) || defined(_M_ARM64) || defined(PUREBYTE_NEON_EMULATION)
#if defined(PUREBYTE_NEON_EMULATION)
#include <simde/arm/neon.h>  // local validation of this file on x86 (tests/local/neon_on_x86.sh; never in the build)
#else
#include <arm_neon.h>
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "kernels/canonical.h"
#include "kernels/kernels.h"

namespace pb::kernels {

namespace {

struct Lanes8 {
    float32x4_t lo, hi;
};

inline Lanes8 zero8() { return {vdupq_n_f32(0.f), vdupq_n_f32(0.f)}; }
inline Lanes8 load8(const float* p) { return {vld1q_f32(p), vld1q_f32(p + 4)}; }
inline void store8(float* p, Lanes8 v) {
    vst1q_f32(p, v.lo);
    vst1q_f32(p + 4, v.hi);
}
// acc + a * b, fused, lane by lane
inline Lanes8 fma8(Lanes8 acc, Lanes8 a, Lanes8 b) {
    return {vfmaq_f32(acc.lo, a.lo, b.lo), vfmaq_f32(acc.hi, a.hi, b.hi)};
}
inline Lanes8 fma8(Lanes8 acc, Lanes8 a, float32x4_t b) {
    return {vfmaq_f32(acc.lo, a.lo, b), vfmaq_f32(acc.hi, a.hi, b)};
}
inline Lanes8 add8(Lanes8 a, Lanes8 b) { return {vaddq_f32(a.lo, b.lo), vaddq_f32(a.hi, b.hi)}; }
inline Lanes8 mul8(Lanes8 a, Lanes8 b) { return {vmulq_f32(a.lo, b.lo), vmulq_f32(a.hi, b.hi)}; }
inline Lanes8 mul8(Lanes8 a, float32x4_t b) { return {vmulq_f32(a.lo, b), vmulq_f32(a.hi, b)}; }

inline float hsum8(Lanes8 v) {
    const float32x4_t q = vaddq_f32(v.lo, v.hi);  // q_i = l_i + l_{i+4}
    const float32x4_t p = vpaddq_f32(q, q);       // [q0+q1, q2+q3, ...]
    return vpadds_f32(vget_low_f32(p));           // (q0+q1) + (q2+q3)
}

// The canonical dot product of one row.
inline float dot1(const float* a, const float* x, int n) {
    Lanes8 s = zero8();
    int i = 0;
    for (; i + 8 <= n; i += 8) s = fma8(s, load8(a + i), load8(x + i));
    float r = hsum8(s);
    for (; i < n; ++i) r = r + a[i] * x[i];
    return r;
}

// Four rows at two positions (n % 8 == 0): each weight vector is loaded once for both positions.
inline void dot4x2(const float* a, int n, const float* x0, const float* x1, float* y0, float* y1) {
    Lanes8 s[4][2];
    for (auto& r : s) r[0] = r[1] = zero8();
    for (int i = 0; i < n; i += 8) {
        const Lanes8 p = load8(x0 + i), q = load8(x1 + i);
        for (int r = 0; r < 4; ++r) {
            const Lanes8 w = load8(a + static_cast<size_t>(r) * n + i);
            s[r][0] = fma8(s[r][0], w, p);
            s[r][1] = fma8(s[r][1], w, q);
        }
    }
    for (int r = 0; r < 4; ++r) {
        y0[r] = hsum8(s[r][0]);
        y1[r] = hsum8(s[r][1]);
    }
}

void fold_rows(const Matrix& W, int o, int count, float* out) {
    const int in = W.in, groups = in / W.group;
    for (int k = 0; k < count; ++k) {
        const int8_t* codes = W.codes + static_cast<size_t>(o + k) * in;
        const float* scales = W.scales + static_cast<size_t>(o + k) * groups;
        float* row = out + static_cast<size_t>(k) * in;
        for (int g = 0; g < groups; ++g) {
            const int b = g * W.group;
            int i = 0;
            for (; i + 8 <= W.group; i += 8) {
                const int16x8_t w16 = vmovl_s8(vld1_s8(codes + b + i));
                vst1q_f32(row + b + i, vmulq_n_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(w16))), scales[g]));
                vst1q_f32(row + b + i + 4, vmulq_n_f32(vcvtq_f32_s32(vmovl_s16(vget_high_s16(w16))), scales[g]));
            }
            for (; i < W.group; ++i) row[b + i] = static_cast<float>(codes[b + i]) * scales[g];
        }
    }
}

void gemm(const Matrix& W, const float* X, size_t ldx, int T, float* Y, size_t ldy, int o0, int o1, float* fold) {
    const int in = W.in;
    int tile = (128 * 1024) / (in * static_cast<int>(sizeof(float)));
    if (tile < 4) tile = 4;
    for (int t0 = 0; t0 < T; t0 += tile) {
        const int t1 = T < t0 + tile ? T : t0 + tile;
        int o = o0;
        for (; o + 4 <= o1; o += 4) {
            const float* rows = W.weights;
            if (rows)
                rows += static_cast<size_t>(o) * in;
            else {
                fold_rows(W, o, 4, fold);
                rows = fold;
            }
            int t = t0;
            if (in % 8 == 0)
                for (; t + 2 <= t1; t += 2)
                    dot4x2(rows, in, X + t * ldx, X + (t + 1) * ldx, Y + t * ldy + o, Y + (t + 1) * ldy + o);
            for (; t < t1; ++t)
                for (int k = 0; k < 4; ++k)
                    Y[t * ldy + o + k] = dot1(rows + static_cast<size_t>(k) * in, X + t * ldx, in);
        }
        for (; o < o1; ++o) {
            const float* row = W.weights;
            if (row)
                row += static_cast<size_t>(o) * in;
            else {
                fold_rows(W, o, 1, fold);
                row = fold;
            }
            for (int t = t0; t < t1; ++t) Y[t * ldy + o] = dot1(row, X + t * ldx, in);
        }
    }
}

void dot_rows(const float* A, size_t lda, int rows, const float* x, int n, float* y) {
    for (int r = 0; r < rows; ++r) y[r] = dot1(A + r * lda, x, n);
}

void rms_inv(const float* X, size_t ldx, int rows, int n, float* inv) {
    scalar_kernels().rms_inv(X, ldx, rows, n, inv);
}

void scale_mul(const float* x, float s, const float* w, float* y, int n) {
    const float32x4_t vs = vdupq_n_f32(s);
    int i = 0;
    for (; i + 4 <= n; i += 4) vst1q_f32(y + i, vmulq_f32(vmulq_f32(vld1q_f32(x + i), vs), vld1q_f32(w + i)));
    for (; i < n; ++i) y[i] = x[i] * s * w[i];
}

// MAXPS / MINPS semantics (the second operand on ties and NaN), which vmaxq/vminq do not have.
inline float32x4_t max_ps(float32x4_t a, float32x4_t b) { return vbslq_f32(vcgtq_f32(a, b), a, b); }
inline float32x4_t min_ps(float32x4_t a, float32x4_t b) { return vbslq_f32(vcltq_f32(a, b), a, b); }

// The Cephes polynomial exp of canonical.h, four lanes at a time.
inline float32x4_t exp_poly(float32x4_t x) {
    x = min_ps(max_ps(x, vdupq_n_f32(-88.3762626647949f)), vdupq_n_f32(88.3762626647949f));
    const float32x4_t fx = vrndmq_f32(vfmaq_f32(vdupq_n_f32(0.5f), x, vdupq_n_f32(1.44269504088896341f)));
    x = vfmsq_f32(x, fx, vdupq_n_f32(0.693359375f));
    x = vfmsq_f32(x, fx, vdupq_n_f32(-2.12194440e-4f));
    const float32x4_t z = vmulq_f32(x, x);
    float32x4_t y = vdupq_n_f32(1.9875691500E-4f);
    y = vfmaq_f32(vdupq_n_f32(1.3981999507E-3f), y, x);
    y = vfmaq_f32(vdupq_n_f32(8.3334519073E-3f), y, x);
    y = vfmaq_f32(vdupq_n_f32(4.1665795894E-2f), y, x);
    y = vfmaq_f32(vdupq_n_f32(1.6666665459E-1f), y, x);
    y = vfmaq_f32(vdupq_n_f32(5.0000001201E-1f), y, x);
    y = vfmaq_f32(vaddq_f32(x, vdupq_n_f32(1.0f)), y, z);
    const int32x4_t e = vshlq_n_s32(vaddq_s32(vcvtq_s32_f32(fx), vdupq_n_s32(0x7f)), 23);
    return vmulq_f32(y, vreinterpretq_f32_s32(e));
}

inline float32x4_t silu4(float32x4_t v) {
    return vdivq_f32(v, vaddq_f32(vdupq_n_f32(1.0f), exp_poly(vsubq_f32(vdupq_n_f32(0.f), v))));
}

void conv_silu(const float* src, size_t lds, int history, float* dst, size_t ldd, const float* kT, const float* bias,
               int t0, int t1, int channels, int K) {
    const int c4 = channels & ~3;
    for (int t = t0; t < t1; ++t) {
        for (int c = 0; c < c4; c += 4) {
            float32x4_t a = vld1q_f32(bias + c);
            for (int k = 0; k < K; ++k) {
                const ptrdiff_t tt = static_cast<ptrdiff_t>(t) - (K - 1) + k;
                if (tt < -history) continue;
                a = vfmaq_f32(a, vld1q_f32(kT + static_cast<size_t>(k) * channels + c),
                              vld1q_f32(src + tt * static_cast<ptrdiff_t>(lds) + c));
            }
            vst1q_f32(dst + t * ldd + c, silu4(a));
        }
        for (int c = c4; c < channels; ++c) {
            float a = bias[c];
            for (int k = 0; k < K; ++k) {
                const ptrdiff_t tt = static_cast<ptrdiff_t>(t) - (K - 1) + k;
                if (tt >= -history)
                    a = canonical::fma_rn(kT[static_cast<size_t>(k) * channels + c],
                                          src[tt * static_cast<ptrdiff_t>(lds) + c], a);
            }
            dst[t * ldd + c] = canonical::silu_poly(a);
        }
    }
}

void silu(const float* x, float* y, int n) {
    int i = 0;
    for (; i + 4 <= n; i += 4) vst1q_f32(y + i, silu4(vld1q_f32(x + i)));
    for (; i < n; ++i) y[i] = canonical::silu_poly(x[i]);
}

void mul_silu(float* y, const float* z, int n) {
    int i = 0;
    for (; i + 4 <= n; i += 4) vst1q_f32(y + i, vmulq_f32(vld1q_f32(y + i), silu4(vld1q_f32(z + i))));
    if (i < n) scalar_kernels().mul_silu(y + i, z + i, n - i);
}

void scan(const ScanBlock& b) {
    if (b.channels != 8) {  // a partial block of the last channels of a head: the specification itself
        scalar_kernels().scan(b);
        return;
    }
    const int N8 = b.N & ~7;
    const float32x4_t vD = vdupq_n_f32(b.D);
    for (int t = 0; t < b.T; ++t) {
        const float32x4_t va = vdupq_n_f32(b.dA[t * b.t_stride]);
        const float32x4_t vdt = vdupq_n_f32(b.dt[t * b.t_stride]);
        const float* B = b.B + t * b.bc_stride;
        const float* C = b.C + t * b.bc_stride;
        const Lanes8 x = load8(b.x + t * b.x_stride);
        const Lanes8 dx = mul8(x, vdt);
        Lanes8 acc[8];
        for (Lanes8& a : acc) a = zero8();
        float* s = b.state;
        for (int n = 0; n < N8; n += 8)
            for (int j = 0; j < 8; ++j) {
                float* sj = s + static_cast<size_t>(n + j) * 8;
                const Lanes8 v = fma8(mul8(load8(sj), va), dx, vdupq_n_f32(B[n + j]));
                store8(sj, v);
                acc[j] = fma8(acc[j], v, vdupq_n_f32(C[n + j]));
            }
        Lanes8 o =
            add8(add8(add8(acc[0], acc[4]), add8(acc[1], acc[5])), add8(add8(acc[2], acc[6]), add8(acc[3], acc[7])));
        for (int n = N8; n < b.N; ++n) {
            float* sn = s + static_cast<size_t>(n) * 8;
            const Lanes8 v = add8(mul8(load8(sn), va), mul8(dx, vdupq_n_f32(B[n])));
            store8(sn, v);
            o = add8(o, mul8(v, vdupq_n_f32(C[n])));
        }
        const Lanes8 d = b.include_D ? mul8(x, vD) : zero8();
        store8(b.y + t * b.y_stride, add8(o, d));
    }
}

const Kernels kNeon = {"neon", gemm, dot_rows, rms_inv, scale_mul, conv_silu, silu, mul_silu, scan};

}  // namespace

const Kernels* neon_kernels() { return &kNeon; }

}  // namespace pb::kernels

#else
#include "kernels/kernels.h"

namespace pb::kernels {
const Kernels* neon_kernels() { return nullptr; }
}  // namespace pb::kernels
#endif
