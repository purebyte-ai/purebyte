// Text helpers of the detection profiles. Several reproduce a precise, documented behaviour (the text a finding
// shows, where a line starts) that users and tests compare byte for byte.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pb::detect {

using Range = std::pair<int64_t, int64_t>;  // [first, second)

// UTF-8 text of bytes [a, z) of `b` (clipped), ill-formed sequences replaced by U+FFFD (see utf8::decode_replace).
std::string text_of(const std::vector<uint8_t>& b, int64_t a, int64_t z, size_t* code_points = nullptr);
// ASCII text: bytes >= 0x80 become U+FFFD.
std::string ascii_text(const uint8_t* p, size_t n);
// `s` without leading and trailing Unicode whitespace (valid UTF-8 in).
std::string strip(const std::string& s);

// The context of a text finding is its line, but never more than kContextMarginBytes on each side of the finding and
// kContextSpanBytes of the finding itself: a document of one huge line (minified code, a data dump) must not cost a
// copy of the line per finding. A line within those bounds is the whole context.
constexpr int64_t kContextMarginBytes = 256;
constexpr int64_t kContextSpanBytes = 4096;

// The context of the finding [a, z) whose line is [line_start, line_end): that line, cut where it goes beyond the
// bounds above (at a UTF-8 character boundary).
Range context_range(const std::vector<uint8_t>& b, int64_t a, int64_t z, int64_t line_start, int64_t line_end);
// The same, finding the line in `b`; the search never goes further than the bounds.
Range context_range(const std::vector<uint8_t>& b, int64_t a, int64_t z);

// A file that starts with a UTF-16 byte order mark and decodes cleanly is re-encoded as UTF-8 (the model reads
// bytes; UTF-16 text would look like binary to it). Returns false, leaving `out` alone, otherwise.
bool utf16_to_utf8(const std::vector<uint8_t>& in, std::vector<uint8_t>& out);

// Newline index of a buffer: line and column of an offset by binary search.
class LineIndex {
public:
    explicit LineIndex(const std::vector<uint8_t>& b);
    // 1-based line and byte column of `pos`, and the bounds [line_start, line_end) of its line (without the \n).
    void locate(int64_t pos, int64_t& line, int64_t& col, int64_t& line_start, int64_t& line_end) const;
    // Where the line holding `pos` starts: 1 + the last line feed before `pos` (0 when there is none).
    int64_t line_start(int64_t pos) const;
    // The first line feed at or after `pos`, else the size of the buffer.
    int64_t next_newline(int64_t pos) const;
    // The line feeds, in order.
    const std::vector<int64_t>& newlines() const { return newlines_; }

private:
    std::vector<int64_t> newlines_;
    int64_t size_ = 0;
};

// Python's bytes.find / rfind with a one-byte needle within [lo, hi); -1 when absent.
int64_t find_byte(const std::vector<uint8_t>& b, uint8_t c, int64_t lo, int64_t hi);
int64_t rfind_byte(const std::vector<uint8_t>& b, uint8_t c, int64_t lo, int64_t hi);
// bytes.find(s, lo)
int64_t find_bytes(const std::vector<uint8_t>& b, const char* s, int64_t lo);

}  // namespace pb::detect
