// File access with UTF-8 paths on every platform (on Windows the narrow fopen would use the ANSI code page and fail
// on paths outside it).
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace pb {

// fopen with a UTF-8 path; nullptr on failure, with errno set (EILSEQ on Windows for a path that is not valid UTF-8).
std::FILE* open_file(const std::string& path, const char* mode);

// Reads a whole file; throws Failure(PB_ERR_IO) when it cannot.
std::vector<uint8_t> read_file(const std::string& path);

}  // namespace pb
