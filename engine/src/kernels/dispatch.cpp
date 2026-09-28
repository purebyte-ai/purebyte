#include <cstring>

#include "core/cpu.h"
#include "kernels/kernels.h"

namespace pb::kernels {

#if !defined(PUREBYTE_HAVE_AVX2)
const Kernels* avx2_kernels() { return nullptr; }
#endif
#if !defined(PUREBYTE_HAVE_NEON)
const Kernels* neon_kernels() { return nullptr; }
#endif

const Kernels* find_kernels(const char* name) {
    const CpuFeatures& cpu = cpu_features();
    const Kernels* avx2 = cpu.avx2_fma ? avx2_kernels() : nullptr;
#if defined(PUREBYTE_NEON_EMULATION)
    const Kernels* neon = neon_kernels();  // the NEON table over SIMDe on x86 (tests/local/neon_on_x86.sh only)
#else
    const Kernels* neon = cpu.neon ? neon_kernels() : nullptr;
#endif
    if (!name || !std::strcmp(name, "auto")) return avx2 ? avx2 : neon ? neon : &scalar_kernels();
    if (!std::strcmp(name, "scalar")) return &scalar_kernels();
    if (!std::strcmp(name, "avx2")) return avx2;
    if (!std::strcmp(name, "neon")) return neon;
    return nullptr;
}

}  // namespace pb::kernels
