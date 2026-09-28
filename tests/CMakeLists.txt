# The test suite (tests/README.md):
#
#   unit        C++ unit tests of the library (tests/cpp): one binary, `purebyte_tests [name-substring | -substring]`
#   detect      the same binary's tests of pb_detect (a separate entry, so that a crash there hides nothing else)
#   models      writes the random tiny models of tests/gen (from spec/FORMAT.md) for the other tests
#   reference   the engine against the NumPy reference in reference/ (skipped when NumPy is missing)
#   golden      the engine against the golden files in tests/golden
#   invariance  every kernel and thread arrangement gives the same bits
#   validation  malformed and contradictory model files are refused with the right status and message
#   cli         end-to-end tests of the `purebyte` binary (when it is built)
#   python      the Python bindings (python/tests) on this build's shared library and CLI (when both are built)
#
#   ctest --test-dir build --build-config Release --output-on-failure

add_executable(purebyte_tests
  cpp/test_main.cpp
  cpp/test_api.cpp
  cpp/test_archive.cpp
  cpp/test_binary_rules.cpp
  cpp/test_detect.cpp
  cpp/test_kernels.cpp
  cpp/test_masking.cpp
  cpp/test_model_limits.cpp
  cpp/test_paths.cpp
  cpp/test_redaction.cpp
  cpp/test_runtime.cpp
  cpp/test_stream.cpp
)
target_link_libraries(purebyte_tests PRIVATE purebyte_static)
purebyte_static_runtime(purebyte_tests)
# The detect layer's tests run as their own ctest entry, so that a failure there never hides the others.
add_test(NAME unit COMMAND purebyte_tests -detect_)
add_test(NAME detect COMMAND purebyte_tests detect_)

# Tools used by the tests and by the measurements of docs/performance.md (not installed).
foreach(tool window_report model_dump window_latency)
  add_executable(${tool} tools/${tool}.cpp)
  target_link_libraries(${tool} PRIVATE purebyte_static)
  purebyte_static_runtime(${tool})
endforeach()

# MinGW: the test programs carry their C++ runtime. Run from a shell whose PATH starts with another MinGW's
# libstdc++-6.dll (Git for Windows has one), a dynamically linked test would load that one and crash.
if(MINGW)
  foreach(target purebyte_tests window_report model_dump window_latency)
    target_link_options(${target} PRIVATE -static)
  endforeach()
endif()

find_package(Python3 3.8 COMPONENTS Interpreter)
if(NOT Python3_FOUND)
  message(WARNING "Python 3.8+ not found: only the C++ unit tests will run (the model-based tests need Python)")
  return()
endif()

set(PB_TEST_WORK "${CMAKE_CURRENT_BINARY_DIR}/work")
add_test(NAME models COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gen/random_models.py ${PB_TEST_WORK}/models)
set_tests_properties(models PROPERTIES FIXTURES_SETUP models ENVIRONMENT PYTHONDONTWRITEBYTECODE=1)
set_tests_properties(unit detect PROPERTIES FIXTURES_REQUIRED models ENVIRONMENT "PUREBYTE_TEST_MODELS=${PB_TEST_WORK}/models")

foreach(name reference golden invariance validation)
  add_test(NAME ${name}
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/python/test_${name}.py
                   --model-dump $<TARGET_FILE:model_dump> --work ${PB_TEST_WORK})
  set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED models SKIP_RETURN_CODE 77
                                          ENVIRONMENT PYTHONDONTWRITEBYTECODE=1)
endforeach()

if(TARGET purebyte_cli)
  add_test(NAME cli
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/python/test_cli.py
                   --purebyte $<TARGET_FILE:purebyte_cli> --work ${PB_TEST_WORK})
  set_tests_properties(cli PROPERTIES FIXTURES_REQUIRED models ENVIRONMENT PYTHONDONTWRITEBYTECODE=1)
endif()

if(TARGET purebyte AND TARGET purebyte_cli)
  add_test(NAME python COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${PROJECT_SOURCE_DIR}/python/tests)
  set_tests_properties(python PROPERTIES ENVIRONMENT
    "PUREBYTE_LIBRARY=$<TARGET_FILE:purebyte>;PUREBYTE_CLI=$<TARGET_FILE:purebyte_cli>;PYTHONDONTWRITEBYTECODE=1")
endif()
