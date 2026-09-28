#include "core/cpu.h"

#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#define PB_X86_64 1
#if defined(_MSC_VER)
#include <immintrin.h>
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

namespace pb {

namespace {

#if defined(PB_X86_64)
void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t r[4]) {
#if defined(_MSC_VER)
    int v[4];
    __cpuidex(v, static_cast<int>(leaf), static_cast<int>(subleaf));
    for (int i = 0; i < 4; ++i) r[i] = static_cast<uint32_t>(v[i]);
#else
    __cpuid_count(leaf, subleaf, r[0], r[1], r[2], r[3]);
#endif
}

uint64_t xgetbv0() {
#if defined(_MSC_VER)
    return _xgetbv(0);
#else
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return (static_cast<uint64_t>(hi) << 32) | lo;
#endif
}
#endif

CpuFeatures detect() {
    CpuFeatures f;
#if defined(PB_X86_64)
    uint32_t r[4];
    cpuid(0, 0, r);
    const uint32_t max_leaf = r[0];
    if (max_leaf >= 7) {
        cpuid(1, 0, r);
        const bool fma = (r[2] >> 12) & 1, osxsave = (r[2] >> 27) & 1, avx = (r[2] >> 28) & 1;
        // The OS must save the XMM and YMM registers on context switches (XCR0 bits 1 and 2).
        const bool ymm_saved = osxsave && (xgetbv0() & 0x6) == 0x6;
        cpuid(7, 0, r);
        const bool avx2 = (r[1] >> 5) & 1;
        f.avx2_fma = fma && avx && avx2 && ymm_saved;
    }
#elif defined(__aarch64__) || defined(_M_ARM64)
    f.neon = true;
#endif
    return f;
}

}  // namespace

const CpuFeatures& cpu_features() {
    static const CpuFeatures features = detect();
    return features;
}

}  // namespace pb
