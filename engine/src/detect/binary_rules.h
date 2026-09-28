// Rules of the `secrets-binary` profile: a credential inside a binary is reported with the printable string around it.
#pragma once

#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

#include "detect/text.h"

namespace pb::detect {

// The run of printable ASCII (0x20..0x7e) around [a, z): what `strings` would show.
Range printable_string_around(const std::vector<uint8_t>& b, int64_t a, int64_t z);

// printable_string_around for the spans of one document met in order (starts and ends increasing): each walk goes on
// from where the previous one stopped, so that all of them read each byte a bounded number of times, however many
// spans share one long string.
class PrintableStrings {
public:
    explicit PrintableStrings(const std::vector<uint8_t>& b) : b_(b) {}
    Range around(int64_t a, int64_t z);

private:
    const std::vector<uint8_t>& b_;
    int64_t last_a_ = -1, last_left_ = 0, last_z_ = -1, last_right_ = 0;
};

// True when [a, z) touches a run of >= L printable ASCII bytes (tab included) or of >= L UTF-16LE characters
// (printable byte + 0x00, at either parity). The runs are measured in the whole buffer; at most L bytes (or
// characters) are read beyond [a, z) on each side.
bool touches_printable_string(const std::vector<uint8_t>& b, int64_t a, int64_t z, int64_t L = 16);

// True when `line`, a printable string of `b` as printable_string_around returns it, is metadata of a JAR manifest or
// signature file: a digest attribute ("SHA-256-Digest: <base64>", "SHA1-Digest-Manifest: ...",
// "SHA-256-Digest-Manifest-Main-Attributes: ..."), the path of an entry ("Name: org/example/Keys.class"), or a
// continuation line of one (manifest lines hold 72 bytes; a longer value goes on in lines that start with one space).
// A digest is a hash of other bytes and a name is a path: neither is ever a credential.
bool is_jar_manifest_metadata(const std::vector<uint8_t>& b, Range line);

// is_jar_manifest_metadata for the spans of one document: every string, and every continuation line walked back
// over, is examined once, however many spans lie in it (a hostile manifest can chain very many continuation lines).
class ManifestLines {
public:
    explicit ManifestLines(const std::vector<uint8_t>& b) : b_(b) {}
    bool metadata(Range line);

private:
    const std::vector<uint8_t>& b_;
    std::map<Range, bool> verdicts_;                         // by the string given
    std::map<std::tuple<int64_t, bool, bool>, bool> walks_;  // by line start and what its continuations hold
};

// True when the ASCII text of [x) and [y) (bytes >= 0x80 shown as U+FFFD, as ascii_text does) is the same.
bool same_ascii_text(const std::vector<uint8_t>& b, Range x, Range y);

}  // namespace pb::detect
