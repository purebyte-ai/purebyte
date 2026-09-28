# PureByte: libpurebyte (engine/), the `purebyte` command-line tool (cli/) and the tests (tests/).
#
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j 4
#   ctest --test-dir build
#
# C++17, no dependencies beyond the vendored single-header cpp-httplib (cli/third_party). Builds with GCC, Clang and
# MSVC on Linux, macOS (arm64 and x86_64) and Windows (MinGW-w64 and MSVC).
cmake_minimum_required(VERSION 3.16)

# macOS: the oldest release the binaries run on. 11.0 is the first with Apple silicon, so arm64, x86_64 and universal
# builds share one minimum (std::filesystem alone needs 10.15). It must be set before project(); pass
# -DCMAKE_OSX_DEPLOYMENT_TARGET=... or set MACOSX_DEPLOYMENT_TARGET to choose another. Ignored on other platforms.
if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND NOT DEFINED ENV{MACOSX_DEPLOYMENT_TARGET})
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version of the binaries")
endif()

project(purebyte VERSION 1.0.0 LANGUAGES CXX)

option(PUREBYTE_BUILD_CLI "Build the purebyte command-line tool" ON)
option(PUREBYTE_BUILD_TESTS "Build the tests and test tools" ON)
option(PUREBYTE_BUILD_SHARED "Build the shared library (purebyte.dll / libpurebyte.so / libpurebyte.dylib)" ON)
option(PUREBYTE_STATIC_RUNTIME "Link the C and C++ runtimes statically into the binaries (release builds)" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------------------------------- numerics flags
# The engine promises the same bits from every kernel on a platform (spec/FORMAT.md, "Numerics"). That requires IEEE
# arithmetic exactly as written: no contraction of a*b + c into a fused multiply-add behind the source's back, and
# no fast-math reassociation. GCC and Clang get -ffp-contract=off. MSVC could contract under /fp:precise before
# Visual Studio 2022: the sources that compute numerics carry `#pragma fp_contract(off)` (engine/src/core/numerics.h),
# and clang-cl, whose /fp:precise contracts, gets -ffp-contract=off too. Under these flags the optimization level does
# not change the results; -O2 is the level of the tests, the golden files and the published measurements.
if(MSVC)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19.14)
    message(FATAL_ERROR "purebyte needs MSVC 19.14 (Visual Studio 2017 version 15.7) or newer: C++17 with <filesystem>")
  endif()
  add_compile_options(/fp:precise /utf-8 /W3 /permissive- /Zc:__cplusplus)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")  # clang-cl
    add_compile_options(/clang:-ffp-contract=off)
  endif()
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  add_compile_options(-ffp-contract=off -fno-fast-math -Wall -Wextra)
  set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
  set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG")
endif()

# ------------------------------------------------------------------------------------------------- target platform
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" PB_PROCESSOR)
set(PB_ARCHS "${CMAKE_OSX_ARCHITECTURES}")
if(NOT PB_ARCHS)
  set(PB_ARCHS "${PB_PROCESSOR}")
endif()
set(PB_BUILD_AVX2 OFF)
set(PB_BUILD_NEON OFF)
foreach(arch IN LISTS PB_ARCHS)
  string(TOLOWER "${arch}" arch)
  if(arch MATCHES "^(x86_64|amd64|x64)$")
    set(PB_BUILD_AVX2 ON)
  elseif(arch MATCHES "^(aarch64|arm64)$")
    set(PB_BUILD_NEON ON)
  endif()
endforeach()
list(LENGTH PB_ARCHS PB_ARCH_COUNT)

# --------------------------------------------------------------------------------------------------------- library
set(PB_ENGINE_SOURCES
  engine/src/api/api_detect.cpp
  engine/src/api/api_engine.cpp
  engine/src/blocks/attention.cpp
  engine/src/blocks/registry.cpp
  engine/src/blocks/ssm.cpp
  engine/src/core/cpu.cpp
  engine/src/core/digest.cpp
  engine/src/core/failure.cpp
  engine/src/core/files.cpp
  engine/src/core/json.cpp
  engine/src/core/threads.cpp
  engine/src/core/utf8.cpp
  engine/src/format/gguf.cpp
  engine/src/heads/bioes.cpp
  engine/src/heads/byte_map.cpp
  engine/src/heads/choice.cpp
  engine/src/heads/exit.cpp
  engine/src/heads/head.cpp
  engine/src/heads/multilabel.cpp
  engine/src/heads/ordinal.cpp
  engine/src/heads/pooling.cpp
  engine/src/heads/registry.cpp
  engine/src/heads/score.cpp
  engine/src/heads/tag.cpp
  engine/src/kernels/dispatch.cpp
  engine/src/kernels/scalar.cpp
  engine/src/model/load_context.cpp
  engine/src/model/lookahead.cpp
  engine/src/model/model.cpp
  engine/src/model/ngram.cpp
  engine/src/model/projection.cpp
  engine/src/runtime/encode.cpp
  engine/src/runtime/forward.cpp
  engine/src/runtime/prefilter.cpp
  engine/src/runtime/scan.cpp
  engine/src/runtime/session.cpp
  engine/src/runtime/stream.cpp
  engine/src/runtime/windows.cpp
  engine/src/detect/archive.cpp
  engine/src/detect/binary_rules.cpp
  engine/src/detect/code_files.cpp
  engine/src/detect/code_rules.cpp
  engine/src/detect/common.cpp
  engine/src/detect/inflate.cpp
  engine/src/detect/inputs.cpp
  engine/src/detect/masking.cpp
  engine/src/detect/profile_generic.cpp
  engine/src/detect/profile_secrets_binary.cpp
  engine/src/detect/profile_secrets_code.cpp
  engine/src/detect/redaction.cpp
  engine/src/detect/registry.cpp
  engine/src/detect/result_json.cpp
  engine/src/detect/spans.cpp
  engine/src/detect/text.cpp
)

set(PB_SIMD_SOURCES)
set(PB_SIMD_DEFINITIONS)
if(PB_BUILD_AVX2)
  list(APPEND PB_SIMD_SOURCES engine/src/kernels/avx2.cpp)
  list(APPEND PB_SIMD_DEFINITIONS PUREBYTE_HAVE_AVX2=1)
  if(MSVC)
    set_source_files_properties(engine/src/kernels/avx2.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
  elseif(APPLE AND PB_ARCH_COUNT GREATER 1)
    set_source_files_properties(engine/src/kernels/avx2.cpp PROPERTIES COMPILE_OPTIONS "-Xarch_x86_64;-mavx2;-Xarch_x86_64;-mfma")
  else()
    set_source_files_properties(engine/src/kernels/avx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma")
  endif()
endif()
if(PB_BUILD_NEON)
  list(APPEND PB_SIMD_SOURCES engine/src/kernels/neon.cpp)
  list(APPEND PB_SIMD_DEFINITIONS PUREBYTE_HAVE_NEON=1)
endif()

include(GNUInstallDirs)

function(purebyte_configure_library target)
  target_include_directories(${target}
    PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/engine/include> $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${PROJECT_SOURCE_DIR}/engine/src)
  target_compile_definitions(${target} PRIVATE PUREBYTE_BUILDING PUREBYTE_VERSION="${PROJECT_VERSION}" ${PB_SIMD_DEFINITIONS})
  find_package(Threads REQUIRED)
  target_link_libraries(${target} PUBLIC Threads::Threads)
endfunction()

# Release binaries can carry the C and C++ runtimes inside (no DLL or shared-library dependencies).
function(purebyte_static_runtime target)
  if(PUREBYTE_STATIC_RUNTIME)
    if(MSVC)
      set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    elseif(NOT APPLE)
      target_link_options(${target} PRIVATE -static)
    endif()
  endif()
endfunction()

# The static library: linked into the CLI and the tests, which may also use the internal headers (in the build tree
# only: an installed copy offers pb.h, like the shared library).
add_library(purebyte_static STATIC ${PB_ENGINE_SOURCES} ${PB_SIMD_SOURCES})
purebyte_configure_library(purebyte_static)
target_include_directories(purebyte_static PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/engine/src>)
set_target_properties(purebyte_static PROPERTIES OUTPUT_NAME purebyte_static)
if(MSVC)
  purebyte_static_runtime(purebyte_static)
endif()

# The shared library: exports only the C API of pb.h.
if(PUREBYTE_BUILD_SHARED)
  add_library(purebyte SHARED ${PB_ENGINE_SOURCES} ${PB_SIMD_SOURCES})
  purebyte_configure_library(purebyte)
  target_compile_definitions(purebyte PUBLIC PUREBYTE_SHARED)
  set_target_properties(purebyte PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
                                            VERSION ${PROJECT_VERSION} SOVERSION 1)
  if(WIN32)
    set_target_properties(purebyte PROPERTIES PREFIX "" RUNTIME_OUTPUT_NAME purebyte)
  endif()
  if(MINGW)
    # A DLL that depends on neither libstdc++-6.dll nor libwinpthread-1.dll.
    target_link_options(purebyte PRIVATE -static-libgcc -static-libstdc++ -Wl,-Bstatic,--whole-archive -lwinpthread
                        -Wl,--no-whole-archive,-Bdynamic)
  endif()
  if(MSVC)
    # With PUREBYTE_STATIC_RUNTIME, a DLL that needs no Visual C++ Redistributable (Python ships no MSVCP140.dll).
    purebyte_static_runtime(purebyte)
  endif()
  install(TARGETS purebyte EXPORT purebyteTargets
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
          LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
          ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

# `cmake --install`: the C API header, the libraries, and a CMake package, so that another project can write
# find_package(purebyte) and link purebyte::purebyte (shared) or purebyte::purebyte_static.
install(FILES engine/include/purebyte/pb.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/purebyte)
install(TARGETS purebyte_static EXPORT purebyteTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(EXPORT purebyteTargets NAMESPACE purebyte:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/purebyte)
include(CMakePackageConfigHelpers)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/purebyteConfig.cmake
     "include(CMakeFindDependencyMacro)\nfind_dependency(Threads)\n"
     "include(\"\${CMAKE_CURRENT_LIST_DIR}/purebyteTargets.cmake\")\n")
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/purebyteConfigVersion.cmake
                                 VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/purebyteConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/purebyteConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/purebyte)

if(PUREBYTE_BUILD_CLI)
  add_subdirectory(cli)
endif()

if(PUREBYTE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

set(PB_KERNELS "scalar")
if(PB_BUILD_AVX2)
  string(APPEND PB_KERNELS ", avx2")
endif()
if(PB_BUILD_NEON)
  string(APPEND PB_KERNELS ", neon")
endif()
message(STATUS "purebyte ${PROJECT_VERSION}: kernels ${PB_KERNELS}; shared library ${PUREBYTE_BUILD_SHARED}, "
               "CLI ${PUREBYTE_BUILD_CLI}, tests ${PUREBYTE_BUILD_TESTS}, static runtime ${PUREBYTE_STATIC_RUNTIME}")
