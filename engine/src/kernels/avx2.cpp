// x86-64 kernels with AVX2 + FMA. Same operations in the same order as the scalar specification (scalar.cpp), so the
// results are the same bits; only the scheduling differs (8 lanes per register, several rows or positions per pass).
//
// This translation unit is compiled with -mavx2 -mfma (or /arch:AVX2) and is only entered after a CPUID check.
// It includes nothing but intrinsics and kernels.h, and everything in it but avx2_kernels() (the entry point, entered
// after that check) has internal linkage: a function shared with other units and compiled here could otherwise be
// picked by the linker and run AVX2 code on a CPU without it.
//
// A universal macOS build compiles every source for every architecture: on the arm64 slice this unit only says that
// there is no AVX2 table.
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>

#include <cstddef>
#include <cstdint>

#include "kernels/kernels.h"

namespace pb::kernels {

namespace {

// ((l0 + l4) + (l1 + l5)) + ((l2 + l6) + (l3 + l7))
inline float hsum8(__m256 v) {
    __m128 q = _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1));
    q = _mm_hadd_ps(q, q);
    q = _mm_hadd_ps(q, q);
    return _mm_cvtss_f32(q);
}

// A mask selecting the first n (0..8) lanes, for loads and stores of a partial vector.
inline __m256i lane_mask(int n) {
    const __m256i index = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    return _mm256_cmpgt_epi32(_mm256_set1_epi32(n), index);
}

// One row: the canonical dot product.
inline float dot1(const float* a, const float* x, int n) {
    __m256 s = _mm256_setzero_ps();
    int i = 0;
    for (; i + 8 <= n; i += 8) s = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(x + i), s);
    float r = hsum8(s);
    for (; i < n; ++i) r = r + a[i] * x[i];
    return r;
}

// Eight rows against one x (n % 8 == 0): eight independent accumulation chains keep both FMA units busy, and the
// eight hsum8 trees are evaluated side by side (lo+hi of each pair of rows, then two 256-bit hadds), which performs
// the same additions as hsum8 per row.
inline void dot8(const float* a, int n, const float* x, float* y) {
    const float *a0 = a, *a1 = a + n, *a2 = a + 2 * n, *a3 = a + 3 * n;
    const float *a4 = a + 4 * n, *a5 = a + 5 * n, *a6 = a + 6 * n, *a7 = a + 7 * n;
    __m256 s0 = _mm256_setzero_ps(), s1 = s0, s2 = s0, s3 = s0, s4 = s0, s5 = s0, s6 = s0, s7 = s0;
    for (int i = 0; i < n; i += 8) {
        const __m256 xv = _mm256_loadu_ps(x + i);
        s0 = _mm256_fmadd_ps(_mm256_loadu_ps(a0 + i), xv, s0);
        s1 = _mm256_fmadd_ps(_mm256_loadu_ps(a1 + i), xv, s1);
        s2 = _mm256_fmadd_ps(_mm256_loadu_ps(a2 + i), xv, s2);
        s3 = _mm256_fmadd_ps(_mm256_loadu_ps(a3 + i), xv, s3);
        s4 = _mm256_fmadd_ps(_mm256_loadu_ps(a4 + i), xv, s4);
        s5 = _mm256_fmadd_ps(_mm256_loadu_ps(a5 + i), xv, s5);
        s6 = _mm256_fmadd_ps(_mm256_loadu_ps(a6 + i), xv, s6);
        s7 = _mm256_fmadd_ps(_mm256_loadu_ps(a7 + i), xv, s7);
    }
    const __m256 u04 = _mm256_add_ps(_mm256_permute2f128_ps(s0, s4, 0x20), _mm256_permute2f128_ps(s0, s4, 0x31));
    const __m256 u15 = _mm256_add_ps(_mm256_permute2f128_ps(s1, s5, 0x20), _mm256_permute2f128_ps(s1, s5, 0x31));
    const __m256 u26 = _mm256_add_ps(_mm256_permute2f128_ps(s2, s6, 0x20), _mm256_permute2f128_ps(s2, s6, 0x31));
    const __m256 u37 = _mm256_add_ps(_mm256_permute2f128_ps(s3, s7, 0x20), _mm256_permute2f128_ps(s3, s7, 0x31));
    _mm256_storeu_ps(y, _mm256_hadd_ps(_mm256_hadd_ps(u04, u15), _mm256_hadd_ps(u26, u37)));
}

// Four rows at two positions (n % 8 == 0): each weight vector is loaded once for both positions. y0 receives the four
// rows at position t, y1 at position t + 1; every output is the canonical dot product.
inline void dot4x2(const float* a, int n, const float* x0, const float* x1, float* y0, float* y1) {
    const float *a0 = a, *a1 = a + n, *a2 = a + 2 * n, *a3 = a + 3 * n;
    __m256 s0 = _mm256_setzero_ps(), s1 = s0, s2 = s0, s3 = s0, s4 = s0, s5 = s0, s6 = s0, s7 = s0;
    for (int i = 0; i < n; i += 8) {
        const __m256 p = _mm256_loadu_ps(x0 + i), q = _mm256_loadu_ps(x1 + i);
        const __m256 w0 = _mm256_loadu_ps(a0 + i), w1 = _mm256_loadu_ps(a1 + i);
        const __m256 w2 = _mm256_loadu_ps(a2 + i), w3 = _mm256_loadu_ps(a3 + i);
        s0 = _mm256_fmadd_ps(w0, p, s0);
        s4 = _mm256_fmadd_ps(w0, q, s4);
        s1 = _mm256_fmadd_ps(w1, p, s1);
        s5 = _mm256_fmadd_ps(w1, q, s5);
        s2 = _mm256_fmadd_ps(w2, p, s2);
        s6 = _mm256_fmadd_ps(w2, q, s6);
        s3 = _mm256_fmadd_ps(w3, p, s3);
        s7 = _mm256_fmadd_ps(w3, q, s7);
    }
    const __m256 u04 = _mm256_add_ps(_mm256_permute2f128_ps(s0, s4, 0x20), _mm256_permute2f128_ps(s0, s4, 0x31));
    const __m256 u15 = _mm256_add_ps(_mm256_permute2f128_ps(s1, s5, 0x20), _mm256_permute2f128_ps(s1, s5, 0x31));
    const __m256 u26 = _mm256_add_ps(_mm256_permute2f128_ps(s2, s6, 0x20), _mm256_permute2f128_ps(s2, s6, 0x31));
    const __m256 u37 = _mm256_add_ps(_mm256_permute2f128_ps(s3, s7, 0x20), _mm256_permute2f128_ps(s3, s7, 0x31));
    const __m256 r = _mm256_hadd_ps(_mm256_hadd_ps(u04, u15), _mm256_hadd_ps(u26, u37));
    _mm_storeu_ps(y0, _mm256_castps256_ps128(r));
    _mm_storeu_ps(y1, _mm256_extractf128_ps(r, 1));
}

// rows [o, o + count) of a ternary matrix, folded to floats: code * scale (exact).
void fold_rows(const Matrix& W, int o, int count, float* out) {
    const int in = W.in, groups = in / W.group;
    for (int k = 0; k < count; ++k) {
        const int8_t* codes = W.codes + static_cast<size_t>(o + k) * in;
        const float* scales = W.scales + static_cast<size_t>(o + k) * groups;
        float* row = out + static_cast<size_t>(k) * in;
        for (int g = 0; g < groups; ++g) {
            const __m256 s = _mm256_set1_ps(scales[g]);
            const int b = g * W.group;
            int i = 0;
            for (; i + 8 <= W.group; i += 8) {
                const __m256i w =
                    _mm256_cvtepi8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(codes + b + i)));
                _mm256_storeu_ps(row + b + i, _mm256_mul_ps(_mm256_cvtepi32_ps(w), s));
            }
            for (; i < W.group; ++i) row[b + i] = static_cast<float>(codes[b + i]) * scales[g];
        }
    }
}

void gemm(const Matrix& W, const float* X, size_t ldx, int T, float* Y, size_t ldy, int o0, int o1, float* fold) {
    const int in = W.in;
    // Positions are processed in tiles whose X rows stay in L2 while the folded rows sweep them.
    int tile = (128 * 1024) / (in * static_cast<int>(sizeof(float)));
    if (tile < 4) tile = 4;
    for (int t0 = 0; t0 < T; t0 += tile) {
        const int t1 = T < t0 + tile ? T : t0 + tile;
        int o = o0;
        for (; o + 8 <= o1; o += 8) {
            const float* rows = W.weights;
            if (rows)
                rows += static_cast<size_t>(o) * in;
            else {
                fold_rows(W, o, 8, fold);
                rows = fold;
            }
            if (in % 8 == 0) {
                int t = t0;
                for (; t + 2 <= t1; t += 2) {
                    const float *xa = X + t * ldx, *xb = xa + ldx;
                    float *ya = Y + t * ldy + o, *yb = ya + ldy;
                    dot4x2(rows, in, xa, xb, ya, yb);
                    dot4x2(rows + 4 * static_cast<size_t>(in), in, xa, xb, ya + 4, yb + 4);
                }
                for (; t < t1; ++t) dot8(rows, in, X + t * ldx, Y + t * ldy + o);
            } else {
                for (int t = t0; t < t1; ++t)
                    for (int k = 0; k < 8; ++k)
                        Y[t * ldy + o + k] = dot1(rows + static_cast<size_t>(k) * in, X + t * ldx, in);
            }
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
    int r = 0;
    if (n % 8 == 0 && lda == static_cast<size_t>(n))
        for (; r + 8 <= rows; r += 8) dot8(A + r * lda, n, x, y + r);
    for (; r < rows; ++r) y[r] = dot1(A + r * lda, x, n);
}

inline float inv_rms_of(double sum, int n) {
    const float mean = static_cast<float>(sum / n) + 1e-5f;
    return 1.0f / _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(mean)));
}

// Sixteen rows at once (n % 4 == 0): each double lane is ONE row summing its squares in element order. A float times
// itself is exact in double, so fma(x, x, s) rounds exactly like s + x*x.
void rms_inv16(const float* X, size_t ld, int n, float* inv) {
    __m256d s0 = _mm256_setzero_pd(), s1 = s0, s2 = s0, s3 = s0;
    for (int i = 0; i < n; i += 4) {
        __m256d* sums[4] = {&s0, &s1, &s2, &s3};
        for (int g = 0; g < 4; ++g) {
            const float* x = X + static_cast<size_t>(4 * g) * ld + i;
            __m128 r0 = _mm_loadu_ps(x), r1 = _mm_loadu_ps(x + ld), r2 = _mm_loadu_ps(x + 2 * ld),
                   r3 = _mm_loadu_ps(x + 3 * ld);
            _MM_TRANSPOSE4_PS(r0, r1, r2, r3);  // r_j = element i + j of the four rows
            __m256d& s = *sums[g];
            __m256d v = _mm256_cvtps_pd(r0);
            s = _mm256_fmadd_pd(v, v, s);
            v = _mm256_cvtps_pd(r1);
            s = _mm256_fmadd_pd(v, v, s);
            v = _mm256_cvtps_pd(r2);
            s = _mm256_fmadd_pd(v, v, s);
            v = _mm256_cvtps_pd(r3);
            s = _mm256_fmadd_pd(v, v, s);
        }
    }
    double s[16];
    _mm256_storeu_pd(s, s0);
    _mm256_storeu_pd(s + 4, s1);
    _mm256_storeu_pd(s + 8, s2);
    _mm256_storeu_pd(s + 12, s3);
    for (int k = 0; k < 16; ++k) inv[k] = inv_rms_of(s[k], n);
}

void rms_inv(const float* X, size_t ldx, int rows, int n, float* inv) {
    int r = 0;
    if (n % 4 == 0)
        for (; r + 16 <= rows; r += 16) rms_inv16(X + r * ldx, ldx, n, inv + r);
    for (; r < rows; ++r) {
        const float* x = X + r * ldx;
        double sum = 0.0;
        for (int i = 0; i < n; ++i) sum += static_cast<double>(x[i]) * static_cast<double>(x[i]);
        inv[r] = inv_rms_of(sum, n);
    }
}

void scale_mul(const float* x, float s, const float* w, float* y, int n) {
    const __m256 vs = _mm256_set1_ps(s);
    int i = 0;
    for (; i + 8 <= n; i += 8)
        _mm256_storeu_ps(y + i, _mm256_mul_ps(_mm256_mul_ps(_mm256_loadu_ps(x + i), vs), _mm256_loadu_ps(w + i)));
    for (; i < n; ++i) y[i] = x[i] * s * w[i];
}

// The Cephes polynomial exp of canonical.h, eight lanes at a time. (The `purebyte:allow` markers accept two of its
// constants, which the secrets-code specialist reads as a secret when it scans this repository.)
inline __m256 exp_poly(__m256 x) {
    x = _mm256_min_ps(_mm256_max_ps(x, _mm256_set1_ps(-88.3762626647949f)), _mm256_set1_ps(88.3762626647949f));
    __m256 fx = _mm256_floor_ps(_mm256_fmadd_ps(x, _mm256_set1_ps(1.44269504088896341f), _mm256_set1_ps(0.5f)));
    x = _mm256_fnmadd_ps(fx, _mm256_set1_ps(0.693359375f), x);
    x = _mm256_fnmadd_ps(fx, _mm256_set1_ps(-2.12194440e-4f), x);
    const __m256 z = _mm256_mul_ps(x, x);
    __m256 y = _mm256_set1_ps(1.9875691500E-4f);
    y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(1.3981999507E-3f));  // purebyte:allow
    y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(8.3334519073E-3f));  // purebyte:allow
    y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(4.1665795894E-2f));
    y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(1.6666665459E-1f));
    y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(5.0000001201E-1f));
    y = _mm256_fmadd_ps(y, z, _mm256_add_ps(x, _mm256_set1_ps(1.0f)));
    const __m256i e = _mm256_slli_epi32(_mm256_add_epi32(_mm256_cvttps_epi32(fx), _mm256_set1_epi32(0x7f)), 23);
    return _mm256_mul_ps(y, _mm256_castsi256_ps(e));
}

inline __m256 silu8(__m256 v) {
    return _mm256_div_ps(v, _mm256_add_ps(_mm256_set1_ps(1.0f), exp_poly(_mm256_sub_ps(_mm256_setzero_ps(), v))));
}

void conv_silu(const float* src, size_t lds, int history, float* dst, size_t ldd, const float* kT, const float* bias,
               int t0, int t1, int channels, int K) {
    for (int t = t0; t < t1; ++t) {
        for (int c = 0; c < channels; c += 8) {
            const int lanes = channels - c < 8 ? channels - c : 8;
            const __m256i m = lane_mask(lanes);
            __m256 a = _mm256_maskload_ps(bias + c, m);
            for (int k = 0; k < K; ++k) {
                const ptrdiff_t tt = static_cast<ptrdiff_t>(t) - (K - 1) + k;
                if (tt < -history) continue;
                a = _mm256_fmadd_ps(_mm256_maskload_ps(kT + static_cast<size_t>(k) * channels + c, m),
                                    _mm256_maskload_ps(src + tt * static_cast<ptrdiff_t>(lds) + c, m), a);
            }
            _mm256_maskstore_ps(dst + t * ldd + c, m, silu8(a));
        }
    }
}

void silu(const float* x, float* y, int n) {
    for (int i = 0; i < n; i += 8) {
        const __m256i m = lane_mask(n - i < 8 ? n - i : 8);
        _mm256_maskstore_ps(y + i, m, silu8(_mm256_maskload_ps(x + i, m)));
    }
}

void mul_silu(float* y, const float* z, int n) {
    for (int i = 0; i < n; i += 8) {
        const __m256i m = lane_mask(n - i < 8 ? n - i : 8);
        const __m256 v = _mm256_maskload_ps(z + i, m);
        const __m256 s =
            _mm256_div_ps(v, _mm256_add_ps(_mm256_set1_ps(1.0f), exp_poly(_mm256_sub_ps(_mm256_setzero_ps(), v))));
        _mm256_maskstore_ps(y + i, m, _mm256_mul_ps(_mm256_maskload_ps(y + i, m), s));
    }
}

// The state of the block is stored n-major with the 8 channels in the lanes: the update is one FMA per n for all
// channels, and the output accumulates in 8 vertical accumulators (acc[n % 8]), exactly the lanes of the canonical
// reduction, combined at the end with the hsum8 tree.
void scan(const ScanBlock& b) {
    if (b.channels != 8) {  // a partial block of the last channels of a head: the specification itself
        scalar_kernels().scan(b);
        return;
    }
    const int N8 = b.N & ~7;
    const __m256 dmask = _mm256_castsi256_ps(_mm256_set1_epi32(b.include_D ? -1 : 0));
    const __m256 vD = _mm256_set1_ps(b.D);
    for (int t = 0; t < b.T; ++t) {
        const __m256 va = _mm256_set1_ps(b.dA[t * b.t_stride]);
        const __m256 vdt = _mm256_set1_ps(b.dt[t * b.t_stride]);
        const float* B = b.B + t * b.bc_stride;
        const float* C = b.C + t * b.bc_stride;
        const __m256 x = _mm256_loadu_ps(b.x + t * b.x_stride);
        const __m256 dx = _mm256_mul_ps(vdt, x);
        __m256 a0 = _mm256_setzero_ps(), a1 = a0, a2 = a0, a3 = a0, a4 = a0, a5 = a0, a6 = a0, a7 = a0;
        float* s = b.state;
        for (int n = 0; n < N8; n += 8) {
#define PB_SCAN_STEP(J, A)                                                                                      \
    {                                                                                                           \
        float* sj = s + static_cast<size_t>(n + J) * 8;                                                         \
        const __m256 v = _mm256_fmadd_ps(dx, _mm256_set1_ps(B[n + J]), _mm256_mul_ps(va, _mm256_loadu_ps(sj))); \
        _mm256_storeu_ps(sj, v);                                                                                \
        A = _mm256_fmadd_ps(v, _mm256_set1_ps(C[n + J]), A);                                                    \
    }
            PB_SCAN_STEP(0, a0)
            PB_SCAN_STEP(1, a1)
            PB_SCAN_STEP(2, a2)
            PB_SCAN_STEP(3, a3)
            PB_SCAN_STEP(4, a4)
            PB_SCAN_STEP(5, a5)
            PB_SCAN_STEP(6, a6)
            PB_SCAN_STEP(7, a7)
#undef PB_SCAN_STEP
        }
        __m256 o = _mm256_add_ps(_mm256_add_ps(_mm256_add_ps(a0, a4), _mm256_add_ps(a1, a5)),
                                 _mm256_add_ps(_mm256_add_ps(a2, a6), _mm256_add_ps(a3, a7)));
        for (int n = N8; n < b.N; ++n) {
            float* sn = s + static_cast<size_t>(n) * 8;
            const __m256 v =
                _mm256_add_ps(_mm256_mul_ps(va, _mm256_loadu_ps(sn)), _mm256_mul_ps(dx, _mm256_set1_ps(B[n])));
            _mm256_storeu_ps(sn, v);
            o = _mm256_add_ps(o, _mm256_mul_ps(v, _mm256_set1_ps(C[n])));
        }
        // o + D*x as a separate product and sum; the AND with the mask sits between them so that no optimisation
        // level can contract the two into one FMA (without D the term is +0).
        const __m256 dterm = _mm256_and_ps(dmask, _mm256_mul_ps(vD, x));
        _mm256_storeu_ps(b.y + t * b.y_stride, _mm256_add_ps(o, dterm));
    }
}

const Kernels kAvx2 = {"avx2", gemm, dot_rows, rms_inv, scale_mul, conv_silu, silu, mul_silu, scan};

}  // namespace

const Kernels* avx2_kernels() { return &kAvx2; }

}  // namespace pb::kernels

#else
#include "kernels/kernels.h"

namespace pb::kernels {
const Kernels* avx2_kernels() { return nullptr; }
}  // namespace pb::kernels
#endif
