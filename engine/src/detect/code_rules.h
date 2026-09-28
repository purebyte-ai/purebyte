// Format rules of the `secrets-code` profile: where a credential in source code or configuration really starts and
// ends, and which parts of a file to look at twice (inside base64) or not at all (data URIs, documentation examples).
//
// These rules were first written as regular expressions; each function here is a hand-written matcher that returns
// exactly the match those expressions return (leftmost start, the backtracking order of greedy and lazy quantifiers),
// which is spelled out next to each one. Every function is covered by tests that fix its results.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "detect/text.h"

namespace pb::detect {

// A base64 run that decodes to readable text: a credential hidden in it is invisible to the model, so the decoded
// bytes are scanned again as a document of their own.
struct Base64Blob {
    int64_t offset = 0;  // of the run in the file
    int64_t length = 0;  // of the run, padding included
    std::vector<uint8_t> decoded;
};

// Runs of >= 32 base64 characters plus up to two '=' that decode (strictly) to text that is at least 12 bytes long
// and more than 90 % printable ASCII, tab, LF or CR. At most `limit` runs are kept: the search stops at the first
// run found after that many.
std::vector<Base64Blob> readable_base64_blobs(const std::vector<uint8_t>& b, int limit = 24);

// True when the decoded bytes are `user:password` (optionally followed by one newline):
//   ^[A-Za-z0-9._@+-]{1,64}:[^\s:]{6,128}$
// Such a blob (a Docker registry `auth` field, for example) is left to the first pass, which reports it whole.
bool is_basic_auth(const std::vector<uint8_t>& decoded);

// PEM private-key blocks: "-----BEGIN <[A-Z0-9 ]*>PRIVATE KEY[ BLOCK]-----" through the first matching END line.
std::vector<Range> private_key_blocks(const std::vector<uint8_t>& b);
// Payloads of base64 data URIs (data:type/subtype[;param]*;base64,PAYLOAD with a payload of >= 16 characters):
// images and fonts, never credentials.
std::vector<Range> data_uri_payloads(const std::vector<uint8_t>& b);

// End of the assignment prefix of a line (`[-+] ["']NAME["'] (:=|=>|=|: )` and blanks), or -1 when the line does
// not start with one. The value is what follows it.
int64_t assignment_end(const uint8_t* s, int64_t n);

// What the rules below need to know about one document, computed once: its lines, where its quotes are, and (on
// demand, once per line) whether a line is an assignment. With it a span costs O(log n) whatever the length of its
// line: a file of one long line (minified code) may hold very many spans.
class CodeIndex {
public:
    CodeIndex(const std::vector<uint8_t>& b, const LineIndex& lines);
    const LineIndex& lines() const { return lines_; }
    // The offsets of '"', '\'' and '`' (kind 0, 1, 2), in order.
    const std::vector<int64_t>& quotes(int kind) const { return quotes_[kind]; }
    // assignment_end of the line that starts at `line_start`.
    int64_t assignment(int64_t line_start) const { return line(line_start).assignment; }
    // The value of that assignment as snap_to_value takes it (trailing blanks off, surrounding quotes off), or
    // {-1, -1} when the line is not an assignment.
    Range value(int64_t line_start) const { return line(line_start).value; }

private:
    struct Line {
        int64_t assignment;
        Range value;
    };
    const Line& line(int64_t line_start) const;

    const std::vector<uint8_t>& b_;
    const LineIndex& lines_;
    std::vector<int64_t> quotes_[3];
    mutable std::map<int64_t, Line> cache_;
};

// Splits a span at every line inside it that starts a new assignment, so that two consecutive `KEY=value` lines
// flagged together become two findings.
std::vector<Range> split_at_assignments(const CodeIndex& index, int64_t a, int64_t z);
// Widens a span to the quoted literal around it (", ' or `) when the quotes are balanced on the line and at most
// `margin` bytes apart; otherwise returns the span unchanged.
Range snap_to_literal(const CodeIndex& index, int64_t a, int64_t z, int64_t margin = 90);
// On a single line that is an assignment, widens a span to the whole value (without surrounding quotes or trailing
// blanks) when the span covers most of it.
Range snap_to_value(const CodeIndex& index, int64_t a, int64_t z);
// The (stripped) line around a span, when the line is at most `maximum` bytes long; otherwise the span. It reads at
// most `maximum` bytes around the span.
Range line_around(const std::vector<uint8_t>& b, int64_t a, int64_t z, int64_t maximum = 120);
// A span that touches a private-key block becomes the union of both (with the first such block). `blocks` are sorted
// and disjoint, as private_key_blocks returns them.
Range extend_to_block(const std::vector<Range>& blocks, int64_t a, int64_t z);
// True when one of `ranges` (sorted and disjoint, as data_uri_payloads returns them) contains [a, z).
bool inside_one_of(const std::vector<Range>& ranges, int64_t a, int64_t z);
// True when the span, with 4 bytes of margin, contains a well-known documentation example key (AWS and Stripe docs).
bool is_documentation_example(const std::vector<uint8_t>& b, int64_t a, int64_t z);

}  // namespace pb::detect
