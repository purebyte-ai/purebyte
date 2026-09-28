#include "detect/archive.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <iterator>
#include <map>
#include <new>

#include "detect/inflate.h"

namespace pb::detect {

namespace {

inline uint32_t le16(const uint8_t* p) { return static_cast<uint32_t>(p[0] | (p[1] << 8)); }
inline uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

inline uint64_t saturating_mul(uint64_t a, uint64_t b) { return a && b > UINT64_MAX / a ? UINT64_MAX : a * b; }
inline uint64_t saturating_add(uint64_t a, uint64_t b) { return b > UINT64_MAX - a ? UINT64_MAX : a + b; }

bool is_zip(const uint8_t* d, size_t n) {
    return n >= 22 && d[0] == 'P' && d[1] == 'K' && ((d[2] == 3 && d[3] == 4) || (d[2] == 5 && d[3] == 6));
}
bool is_gzip(const uint8_t* d, size_t n) { return n >= 18 && d[0] == 0x1f && d[1] == 0x8b && d[2] == 8; }

// Octal number of a tar header field (or base-256 when the first byte has its top bit set); -1 when invalid.
int64_t tar_number(const uint8_t* p, size_t n) {
    if (p[0] & 0x80) {
        int64_t v = p[0] & 0x7f;
        for (size_t i = 1; i < n; ++i) {
            if (v > (INT64_MAX >> 8)) return -1;
            v = (v << 8) | p[i];
        }
        return v;
    }
    int64_t v = 0;
    size_t i = 0;
    while (i < n && p[i] == ' ') ++i;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; ++i) {
        if (v > (INT64_MAX >> 3)) return -1;
        v = v * 8 + (p[i] - '0');
    }
    return v;
}

bool tar_header_valid(const uint8_t* h) {
    int64_t sum = 0;
    for (int i = 0; i < 512; ++i) sum += (i >= 148 && i < 156) ? ' ' : h[i];
    return tar_number(h + 148, 8) == sum;
}

bool is_tar(const uint8_t* d, size_t n) {
    return n >= 512 && std::memcmp(d + 257, "ustar", 5) == 0 && tar_header_valid(d);
}

std::string field(const uint8_t* p, size_t n) {
    size_t len = 0;
    while (len < n && p[len]) ++len;
    return std::string(reinterpret_cast<const char*>(p), len);
}

// The `path` of a pax extended header (records "<length> <key>=<value>\n"), or "" when it has none or is malformed.
std::string pax_path(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n;) {
        size_t j = i, length = 0;
        while (j < n && p[j] >= '0' && p[j] <= '9' && length <= n) length = length * 10 + (p[j++] - '0');
        if (j == i || j >= n || p[j] != ' ' || length > n - i || length < j - i + 2 || p[i + length - 1] != '\n')
            return "";
        const uint8_t* key = p + j + 1;
        const uint8_t* end = p + i + length - 1;
        if (end - key > 5 && std::memcmp(key, "path=", 5) == 0)
            return std::string(reinterpret_cast<const char*>(key + 5), reinterpret_cast<const char*>(end));
        i += length;
    }
    return "";
}

// `s` cut in its middle to at most `limit` bytes: the first quarter and the end are kept around a "…" (UTF-8), and a
// UTF-8 character is never cut in two.
std::string shorten(const std::string& s, size_t limit) {
    if (s.size() <= limit) return s;
    const char marker[] = "\xE2\x80\xA6";
    const size_t keep = limit - (sizeof marker - 1);
    auto continuation = [&](size_t i) { return (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80; };
    size_t head = keep / 4, tail = s.size() - (keep - keep / 4);
    for (int k = 0; k < 3 && head > 0 && continuation(head); ++k) --head;
    for (int k = 0; k < 3 && tail < s.size() && continuation(tail); ++k) ++tail;
    return s.substr(0, head) + marker + s.substr(tail);
}

// Members may not share bytes. `ranges` holds the [start, end) of the members read so far (start -> end); true, and
// [start, end) added, when it shares none with them.
bool claim(std::map<size_t, size_t>& ranges, size_t start, size_t end) {
    const auto next = ranges.lower_bound(start);
    if (next != ranges.end() && next->first < end) return false;
    if (next != ranges.begin() && std::prev(next)->second > start) return false;
    ranges.emplace(start, end);
    return true;
}

class Expander {
public:
    Expander(const ArchiveLimits& limits, const MemberFilter& wanted, std::vector<ScanFailure>& failures)
        : limits_(limits), wanted_(wanted), failures_(failures) {
        limits_.max_depth = std::min(limits_.max_depth, kArchiveMaxDepth);
    }

    // The outer archive. Whatever goes wrong inside it is one of its failures; failures over the limit are counted in
    // a last one.
    void expand_outer(const std::string& name, const uint8_t* data, size_t size) {
        outer_ = name.size();
        expand(name, data, size, 1);
        if (unlisted_)
            failures_.push_back({name, std::to_string(unlisted_) + " more failures inside this archive (not listed)"});
    }

    std::vector<ArchiveMember> members;

private:
    void expand(const std::string& name, const uint8_t* data, size_t size, int depth) {
        try {
            if (is_zip(data, size))
                zip(name, data, size, depth);
            else if (is_gzip(data, size))
                gzip(name, data, size, depth);
            else if (is_tar(data, size))
                tar(name, data, size, depth);
        } catch (const std::bad_alloc&) {
            failure(name, "not enough memory to expand it: the rest was not scanned");
        } catch (const std::exception&) {
            failure(name, "malformed archive: the rest was not scanned");
        }
    }

    // Failures are listed up to kArchiveMaxFailures per outer archive, then only counted. What is kept (names of
    // members and failures, decompressed bytes) is charged to the archive's budget.
    void failure(const std::string& name, const std::string& why) {
        if (listed_ == kArchiveMaxFailures) {
            ++unlisted_;
            return;
        }
        ++listed_;
        charge(name.size() + why.size());
        failures_.push_back({name, why});
    }
    bool wanted(const std::string& name) const { return !wanted_ || wanted_(leaf_name(name)); }
    void charge(uint64_t bytes) { total_ = saturating_add(total_, bytes); }

    // "archive!/entry". The part after the outer archive's own name is at most kArchiveMaxNameBytes: a longer one keeps
    // its start and its end (where the file name the filter reads is).
    std::string member_name(const std::string& archive, const std::string& entry) const {
        return archive.substr(0, outer_) + shorten(archive.substr(outer_) + "!/" + entry, kArchiveMaxNameBytes);
    }

    // Members are counted as they are met, wanted or not, so that a flood of tiny entries costs nothing.
    bool count_member(const std::string& archive) {
        if (++seen_ <= limits_.max_members) return true;
        failure(archive, "more than " + std::to_string(limits_.max_members) + " members: the rest was not scanned");
        return false;
    }

    // Every byte decompressed counts against the total budget, whether its member was kept, refused or corrupt: once it
    // is spent, nothing more is extracted from the outer archive.
    bool within_budget(const std::string& archive) {
        if (total_ < limits_.max_total_bytes) return true;
        failure(archive, "over the limit of " + std::to_string(limits_.max_total_bytes) +
                             " bytes decompressed from one archive (possible archive bomb): the rest was not scanned");
        return false;
    }

    // Output limit of one member: the size limit, what is left of the total budget, and the ratio limit.
    size_t budget(uint64_t compressed) const {
        const uint64_t by_ratio = saturating_add(saturating_mul(compressed, limits_.max_ratio), 1u << 20);
        const uint64_t left = limits_.max_total_bytes > total_ ? limits_.max_total_bytes - total_ : 0;
        return static_cast<size_t>(
            std::min({limits_.max_member_bytes, left, by_ratio, static_cast<uint64_t>(SIZE_MAX)}));
    }

    // Decompresses the raw DEFLATE data of a member (charging what came out, whatever the outcome); false, with the
    // failure reported, unless it went well.
    bool inflated(const std::string& name, const uint8_t* p, size_t n, size_t limit, std::vector<uint8_t>& out,
                  size_t* used = nullptr) {
        const size_t before = out.size();
        const InflateResult r = inflate(p, n, limit, out, used);
        charge(out.size() - before);
        if (r == InflateResult::TooLarge)
            failure(name, "decompressed size over the limit (possible archive bomb): not scanned");
        if (r == InflateResult::Corrupt) failure(name, "corrupt compressed data: not scanned");
        return r == InflateResult::Ok;
    }

    // A member the filter does not want is only extracted when it could be an archive: its first bytes decide.
    bool starts_like_archive(const uint8_t* p, size_t n, bool deflated) {
        if (!deflated) return looks_like_archive(p, n);
        std::vector<uint8_t> head;
        inflate(p, n, 512, head);  // stopping at 512 bytes (TooLarge) is the normal outcome here
        charge(head.size());
        return looks_like_archive(head);
    }

    // An extracted member: expanded again when it is an archive (within the depth limit), else kept when wanted.
    void add(std::string name, std::vector<uint8_t> bytes, int depth) {
        if (looks_like_archive(bytes)) {
            if (depth < limits_.max_depth) return expand(name, bytes.data(), bytes.size(), depth + 1);
            if (!wanted(name)) return failure(name, "archive nested deeper than the limit: not scanned");
            failure(name, "archive nested deeper than the limit: its members were not scanned");
        } else if (!wanted(name)) {
            return;
        }
        charge(name.size());
        members.push_back({std::move(name), std::move(bytes)});
    }

    void zip(const std::string& name, const uint8_t* d, size_t n, int depth) {
        // The end-of-central-directory record: the last "PK\5\6" within the final 64 KiB + 22 bytes.
        size_t eocd = n;
        for (size_t i = n - 22 + 1; i-- > (n > 65557 ? n - 65557 : 0);)
            if (le32(d + i) == 0x06054b50) {
                eocd = i;
                break;
            }
        if (eocd == n) return failure(name, "zip without a central directory: not scanned");
        const uint32_t entries = le16(d + eocd + 10), directory = le32(d + eocd + 16);
        if (entries == 0xFFFF || directory == 0xFFFFFFFFu)
            return failure(name, "zip64 archives are not supported: not scanned");
        std::map<size_t, size_t> read;  // the bytes of the members met so far: local header and data
        size_t p = directory;
        for (uint32_t e = 0; e < entries; ++e) {
            if (p + 46 > n || le32(d + p) != 0x02014b50)
                return failure(name, "corrupt zip central directory: the rest was not scanned");
            const uint8_t* h = d + p;
            const uint32_t flags = le16(h + 8), method = le16(h + 10), compressed = le32(h + 20);
            const uint32_t name_length = le16(h + 28), extra_length = le16(h + 30), comment_length = le16(h + 32);
            const uint32_t local = le32(h + 42);
            if (p + 46 + name_length > n)
                return failure(name, "corrupt zip central directory: the rest was not scanned");
            const std::string entry(reinterpret_cast<const char*>(h + 46), name_length);
            p += 46 + static_cast<size_t>(name_length) + extra_length + comment_length;
            if (entry.empty() || entry.back() == '/') continue;  // a directory
            if (!count_member(name) || !within_budget(name)) return;
            const std::string member = member_name(name, entry);
            const bool keep = wanted(member);
            if (flags & 1) {
                if (keep) failure(member, "encrypted: not scanned");
                continue;
            }
            if (method != 0 && method != 8) {
                if (keep)
                    failure(member, "compression method " + std::to_string(method) + " is not supported: not scanned");
                continue;
            }
            if (static_cast<size_t>(local) + 30 > n || le32(d + local) != 0x04034b50) {
                failure(member, "corrupt local header: not scanned");
                continue;
            }
            const size_t at = local + 30 + static_cast<size_t>(le16(d + local + 26)) + le16(d + local + 28);
            if (at > n || compressed > n - at) {
                failure(member, "truncated: not scanned");
                continue;
            }
            // Central directory entries that point at the same bytes (or into each other's) would have one small file
            // decompressed again and again.
            if (!claim(read, local, at + compressed)) {
                failure(member, "overlaps another member (possible archive bomb): not scanned");
                continue;
            }
            if (!keep && !starts_like_archive(d + at, compressed, method == 8)) continue;
            std::vector<uint8_t> bytes;
            if (method == 0) {
                if (compressed > budget(compressed)) {
                    failure(member, "over the size limit: not scanned");
                    continue;
                }
                bytes.assign(d + at, d + at + compressed);
                charge(bytes.size());
            } else if (!inflated(member, d + at, compressed, budget(compressed), bytes)) {
                continue;
            }
            add(member, std::move(bytes), depth);
        }
    }

    // A gzip file may hold several members one after the other (concatenated files, parallel compressors): their
    // contents form one stream.
    void gzip(const std::string& name, const uint8_t* d, size_t n, int depth) {
        if (!within_budget(name)) return;
        std::string inner;
        std::vector<uint8_t> bytes;
        const size_t limit = budget(n);
        for (size_t p = 0; p + 18 <= n && is_gzip(d + p, n - p);) {
            const uint8_t flags = d[p + 3];
            size_t q = p + 10;
            if (flags & 4) q += 2 + (q + 2 <= n ? le16(d + q) : 0);               // FEXTRA
            if (q > n) return failure(name, "corrupt gzip header: not scanned");  // an extra field past the end
            if (flags & 8) {                                                      // FNAME
                const size_t start = q;
                while (q < n && d[q]) ++q;
                if (p == 0) inner = std::string(reinterpret_cast<const char*>(d + start), q - start);
                ++q;
            }
            if (flags & 16) {  // FCOMMENT
                while (q < n && d[q]) ++q;
                ++q;
            }
            if (flags & 2) q += 2;  // FHCRC
            if (q >= n) return failure(name, "corrupt gzip header: not scanned");
            size_t used = 0;
            if (!inflated(name, d + q, n - q, limit, bytes, &used)) return;
            p = q + used + 8;  // the CRC-32 and size trailer
        }
        if (inner.empty()) {  // "x.tar.gz" holds "x.tar", "x.tgz" holds "x.tar", "x.gz" holds "x"
            inner = leaf_name(name);
            const size_t dot = inner.rfind('.');
            if (dot != std::string::npos)
                inner = inner.substr(dot) == ".tgz" ? inner.substr(0, dot) + ".tar" : inner.substr(0, dot);
        }
        add(member_name(name, inner), std::move(bytes), depth);
    }

    void tar(const std::string& name, const uint8_t* d, size_t n, int depth) {
        std::string long_name;  // from a GNU `L` or a pax `x` header, for the next member
        for (size_t p = 0; p + 512 <= n;) {
            const uint8_t* h = d + p;
            if (std::all_of(h, h + 512, [](uint8_t c) { return c == 0; })) break;  // end of archive
            if (!tar_header_valid(h)) return failure(name, "corrupt tar header: the rest was not scanned");
            const int64_t size = tar_number(h + 124, 12);
            if (size < 0 || static_cast<uint64_t>(size) > n - p - 512)
                return failure(name, "truncated tar member: the rest was not scanned");
            const uint8_t* data = h + 512;
            const size_t length = static_cast<size_t>(size);
            const char type = static_cast<char>(h[156]);
            if (type == 'L') {
                long_name = shorten(field(data, length), kArchiveMaxNameBytes);
            } else if (type == 'x') {
                const std::string path = pax_path(data, length);
                if (!path.empty()) long_name = shorten(path, kArchiveMaxNameBytes);
            } else if (type == '0' || type == '\0' || type == '7') {  // regular files
                std::string entry = field(h, 100);
                if (std::memcmp(h + 257, "ustar", 6) == 0 && h[345])
                    entry = field(h + 345, 155) + "/" + entry;  // POSIX prefix
                const std::string member = member_name(name, long_name.empty() ? entry : long_name);
                long_name.clear();
                if (!count_member(name) || !within_budget(name)) return;
                if (wanted(member) || looks_like_archive(data, length)) {
                    if (length > budget(length)) {
                        failure(member, "over the size limit: not scanned");
                    } else {
                        charge(length);
                        add(member, std::vector<uint8_t>(data, data + length), depth);
                    }
                }
            } else if (type != 'g') {
                long_name.clear();  // links, directories and devices carry no content to scan
            }
            p += 512 + (length + 511) / 512 * 512;
        }
    }

    ArchiveLimits limits_;
    const MemberFilter& wanted_;
    std::vector<ScanFailure>& failures_;
    uint64_t total_ = 0;   // the budget spent: bytes decompressed or copied, names kept
    size_t seen_ = 0;      // members met
    size_t outer_ = 0;     // length of the outer archive's name, the start of every display name
    size_t listed_ = 0;    // failures reported
    size_t unlisted_ = 0;  // and the ones over the limit
};

}  // namespace

bool looks_like_archive(const uint8_t* data, size_t size) {
    return data && (is_zip(data, size) || is_gzip(data, size) || is_tar(data, size));
}

std::string leaf_name(const std::string& name) {
    const size_t cut = name.find_last_of("/\\!");
    return cut == std::string::npos ? name : name.substr(cut + 1);
}

std::vector<ArchiveMember> expand_archive(const std::string& name, const uint8_t* data, size_t size,
                                          const ArchiveLimits& limits, const MemberFilter& wanted,
                                          std::vector<ScanFailure>& failures) {
    Expander e(limits, wanted, failures);
    if (data) e.expand_outer(name, data, size);
    return std::move(e.members);
}

}  // namespace pb::detect
