# The `purebyte` command-line tool: one self-contained binary.

# The catalog of released models (models/models.json) is compiled in, so `purebyte models` works offline.
set(PB_CATALOG_JSON "${PROJECT_SOURCE_DIR}/models/models.json")
if(EXISTS "${PB_CATALOG_JSON}")
  file(READ "${PB_CATALOG_JSON}" PB_CATALOG)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PB_CATALOG_JSON}")
else()
  set(PB_CATALOG "{\"schema\": 1, \"models\": []}")
endif()
string(LENGTH "${PB_CATALOG}" PB_CATALOG_LENGTH)
if(PB_CATALOG_LENGTH GREATER 16000)
  message(FATAL_ERROR "models/models.json is too large to embed as one string literal; split it in catalog.cpp.in")
endif()
configure_file(catalog_data.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/catalog_data.cpp" @ONLY)

add_executable(purebyte_cli
  main.cpp
  args.cpp
  catalog.cpp
  child_process.cpp
  cmd_bench.cpp
  cmd_decide.cpp
  cmd_info.cpp
  cmd_models.cpp
  cmd_redact.cpp
  cmd_scan.cpp
  cmd_serve.cpp
  decision.cpp
  detection.cpp
  engine.cpp
  http_request.cpp
  exclusions.cpp
  inputs.cpp
  output.cpp
  sarif.cpp
  setup.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/catalog_data.cpp"
)
set_target_properties(purebyte_cli PROPERTIES OUTPUT_NAME purebyte)
target_include_directories(purebyte_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(purebyte_cli PRIVATE purebyte_static)
if(WIN32)
  target_compile_definitions(purebyte_cli PRIVATE _WIN32_WINNT=0x0A00)  # cpp-httplib needs the Windows 10 API level
  target_link_libraries(purebyte_cli PRIVATE ws2_32 shell32)
endif()
if(MINGW)
  # One self-contained executable: no libstdc++-6.dll, libgcc_s_seh-1.dll or libwinpthread-1.dll to ship next to it.
  target_link_options(purebyte_cli PRIVATE -static)
else()
  purebyte_static_runtime(purebyte_cli)
endif()

include(GNUInstallDirs)
install(TARGETS purebyte_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# The licenses that go with the binary: the project's, and that of the vendored HTTP library (VENDORED.md), as in the
# release archives.
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(FILES third_party/cpp-httplib/LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR} RENAME LICENSE-cpp-httplib)
