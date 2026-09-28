// The numerics contract (spec/FORMAT.md, section 5) forbids contracting a * b + c into a fused multiply-add: every
// source that computes model numerics includes this header before its first function. GCC and Clang, clang-cl
// included, get -ffp-contract=off from CMakeLists.txt; MSVC could contract under /fp:precise before Visual Studio 2022,
// and this pragma turns contraction off for the rest of the translation unit on every version.
#pragma once

#if defined(_MSC_VER) && !defined(__clang__)
#pragma fp_contract(off)
#endif
