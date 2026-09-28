// UTF-8 helpers with the exact semantics the output format relies on.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace pb::utf8 {

// Appends the UTF-8 encoding of code point `cp`.
void append(std::string& out, uint32_t cp);

// Decodes bytes as UTF-8 replacing every maximal ill-formed subsequence with one U+FFFD (the "best practice" of the
// Unicode standard, as CPython's bytes.decode("utf-8", "replace") does, including a sequence cut by the end of the
// data). `code_points`, when given, receives the number of code points of the result.
std::string decode_replace(const uint8_t* data, size_t size, size_t* code_points = nullptr);

// `s` with every ill-formed sequence replaced as in decode_replace (identity on valid UTF-8).
std::string sanitize(const std::string& s);

// `s` sanitized, with every control character (C0, DEL and C1) written as an escape (\x1b, \u009b): safe to print on a
// terminal whatever bytes a file put in it (error messages quote names read from model files).
std::string printable(const std::string& s);

// Number of code points of VALID UTF-8.
size_t count_code_points(const std::string& valid);

}  // namespace pb::utf8
