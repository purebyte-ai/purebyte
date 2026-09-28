// What the CPU running the process can execute. Detected once, at run time: the library is compiled for the
// baseline of its architecture and chooses its SIMD kernels here.
#pragma once

namespace pb {

struct CpuFeatures {
    bool avx2_fma = false;  // x86-64: AVX2 + FMA3, and the OS saves the 256-bit registers
    bool neon = false;      // AArch64: Advanced SIMD (always present)
};

const CpuFeatures& cpu_features();

}  // namespace pb
