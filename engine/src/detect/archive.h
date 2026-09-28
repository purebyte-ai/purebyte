// Archives are scanned inside: zip and its relatives (jar, apk, war, whl, nupkg, docx...), gzip and tar, recursively.
// Every member is untrusted: its name is only a label (nothing is ever written to disk), and depth, member size,
// total work, compression ratio, name length and the number of failures listed are limited so that an archive bomb
// costs nothing. zip members that share bytes (central directory entries pointing at the same data) are refused.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "detect/document.h"
#include "purebyte/pb.h"

namespace pb::detect {

// Limits no caller can raise. Deeper nesting would be stopped by the stack rather than by a setting; a display name is
// only a label (a longer one keeps its start and its end around a "…"); more failures than this in one outer archive
// are counted in a last one instead of listed.
constexpr int kArchiveMaxDepth = 16;
constexpr size_t kArchiveMaxNameBytes = 4096;  // of a member's display name, after its outer archive's name
constexpr size_t kArchiveMaxFailures = 1000;

// The defaults are those of pb_archive_limits_init (purebyte/pb.h).
struct ArchiveLimits {
    int max_depth = PB_ARCHIVE_MAX_DEPTH;                     // archives inside archives (at most kArchiveMaxDepth)
    uint64_t max_member_bytes = PB_ARCHIVE_MAX_MEMBER_BYTES;  // one decompressed member
    uint64_t max_total_bytes = PB_ARCHIVE_MAX_TOTAL_BYTES;    // the work of one outer archive: every byte decompressed
                                                              // from it, kept or not, and the names it reports
    uint64_t max_ratio = PB_ARCHIVE_MAX_RATIO;                // decompressed / compressed, per member (plus 1 MiB)
    size_t max_members = 100000;                              // entries met, wanted or not
};

struct ArchiveMember {
    std::string name;  // "outer.jar!/inner/path", nested members "a.zip!/b.jar!/c.class"
    std::vector<uint8_t> bytes;
};

// True when `data` starts like a zip, gzip or tar archive.
bool looks_like_archive(const uint8_t* data, size_t size);
inline bool looks_like_archive(const std::vector<uint8_t>& data) {
    return looks_like_archive(data.data(), data.size());
}

// Decides by its file name (the part after the last '/' or '!') whether a member is worth extracting. Members that
// are archives themselves are expanded whatever their name.
using MemberFilter = std::function<bool(const std::string& file_name)>;

// The leaf members of the archive `data` that `wanted` accepts (nullptr: all), members that are archives expanded in
// turn up to the depth limit. Members that could not be extracted (unsupported method, encrypted, corrupt, over a
// limit) are reported in `failures` and skipped. Never throws on a malformed archive: that is a failure too.
std::vector<ArchiveMember> expand_archive(const std::string& name, const uint8_t* data, size_t size,
                                          const ArchiveLimits& limits, const MemberFilter& wanted,
                                          std::vector<ScanFailure>& failures);

// The file name of a display name: what follows the last '/', '\' or '!'.
std::string leaf_name(const std::string& name);

}  // namespace pb::detect
