// Hostile archives (detect/archive.h, pb.h: pb_archive_expand): the work of an archive is bounded whatever its central
// directory says, names are labels of bounded length, failures are listed within a limit, a malformed header is a
// failure and never an exception, and the limits a caller passes are clamped. Every archive is built here, in memory.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "detect/archive.h"
#include "purebyte/pb.h"
#include "test.h"

using pb::detect::ArchiveLimits;
using pb::detect::ArchiveMember;
using pb::detect::expand_archive;
using pb::detect::ScanFailure;

namespace {

using Bytes = std::vector<uint8_t>;

void put16(Bytes& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
}
void put32(Bytes& out, uint32_t v) {
    put16(out, v & 0xFFFF);
    put16(out, v >> 16);
}
void append(Bytes& out, const std::string& s) { out.insert(out.end(), s.begin(), s.end()); }
void append(Bytes& out, const Bytes& b) { out.insert(out.end(), b.begin(), b.end()); }

// DEFLATE bits, least significant first; Huffman codes most significant bit first (RFC 1951, 3.1.1).
struct BitWriter {
    Bytes out;
    uint32_t byte = 0;
    int used = 0;
    void bits(uint32_t v, int n) {
        for (int i = 0; i < n; ++i) {
            byte |= ((v >> i) & 1u) << used;
            if (++used == 8) {
                out.push_back(static_cast<uint8_t>(byte));
                byte = 0;
                used = 0;
            }
        }
    }
    void code(uint32_t v, int n) {
        for (int i = n - 1; i >= 0; --i) bits((v >> i) & 1u, 1);
    }
    Bytes take() {
        if (used) out.push_back(static_cast<uint8_t>(byte));
        return out;
    }
};

// `n` zero bytes as one fixed-Huffman block: a literal 0, then copies of 258 bytes at distance 1 (13 bits each).
Bytes deflate_zeros(size_t n) {
    BitWriter w;
    w.bits(1, 1);  // the last block
    w.bits(1, 2);  // fixed codes
    size_t left = n;
    if (left) {
        w.code(0x30, 8);  // literal 0
        --left;
    }
    for (; left >= 258; left -= 258) {
        w.code(0xC5, 8);  // length 258 (symbol 285)
        w.code(0, 5);     // distance 1
    }
    for (; left; --left) w.code(0x30, 8);
    w.code(0, 7);  // end of block
    return w.take();
}

// Raw DEFLATE of `data` in stored blocks (no compression).
Bytes deflate_stored(const Bytes& data) {
    Bytes out;
    size_t at = 0;
    do {
        const size_t n = std::min<size_t>(data.size() - at, 65535);
        out.push_back(static_cast<uint8_t>(at + n == data.size() ? 1 : 0));
        put16(out, static_cast<uint32_t>(n));
        put16(out, static_cast<uint32_t>(n ^ 0xFFFF));
        out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(at),
                   data.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
    } while (at < data.size());
    return out;
}

Bytes gzip(const Bytes& data, const std::string& file_name = "") {
    Bytes out = {0x1f, 0x8b, 8, static_cast<uint8_t>(file_name.empty() ? 0 : 8), 0, 0, 0, 0, 0, 255};
    if (!file_name.empty()) {
        append(out, file_name);
        out.push_back(0);
    }
    append(out, deflate_stored(data));
    for (int i = 0; i < 8; ++i) out.push_back(0);  // CRC-32 and size: not checked
    return out;
}

// A zip local header followed by the member's data.
Bytes local_entry(const std::string& name, const Bytes& data, uint32_t method) {
    Bytes out;
    put32(out, 0x04034b50);
    put16(out, 20);
    put16(out, 0);  // flags
    put16(out, method);
    put32(out, 0);  // time, date
    put32(out, 0);  // CRC-32
    put32(out, static_cast<uint32_t>(data.size()));
    put32(out, 0);  // uncompressed size: not read
    put16(out, static_cast<uint32_t>(name.size()));
    put16(out, 0);
    append(out, name);
    append(out, data);
    return out;
}

// A zip file: local entries first, then any central directory entries (pointing where the test wants).
class Zip {
public:
    uint32_t local(const std::string& name, const Bytes& data, uint32_t method) {
        const uint32_t at = static_cast<uint32_t>(body_.size());
        append(body_, local_entry(name, data, method));
        return at;
    }
    void entry(const std::string& name, uint32_t local, uint32_t compressed, uint32_t method, uint32_t flags = 0) {
        put32(directory_, 0x02014b50);
        put16(directory_, 20);
        put16(directory_, 20);
        put16(directory_, flags);
        put16(directory_, method);
        put32(directory_, 0);
        put32(directory_, 0);
        put32(directory_, compressed);
        put32(directory_, 0);
        put16(directory_, static_cast<uint32_t>(name.size()));
        put16(directory_, 0);  // extra
        put16(directory_, 0);  // comment
        put16(directory_, 0);  // disk
        put16(directory_, 0);  // internal attributes
        put32(directory_, 0);  // external attributes
        put32(directory_, local);
        append(directory_, name);
        ++entries_;
    }
    void add(const std::string& name, const Bytes& data, uint32_t method) {
        entry(name, local(name, data, method), static_cast<uint32_t>(data.size()), method);
    }
    Bytes bytes() const {
        Bytes out = body_;
        const uint32_t directory = static_cast<uint32_t>(out.size());
        append(out, directory_);
        put32(out, 0x06054b50);
        put32(out, 0);  // disk numbers
        put16(out, entries_);
        put16(out, entries_);
        put32(out, static_cast<uint32_t>(directory_.size()));
        put32(out, directory);
        put16(out, 0);
        return out;
    }

private:
    Bytes body_, directory_;
    uint32_t entries_ = 0;
};

void tar_header(Bytes& out, const std::string& name, size_t size, char type) {
    uint8_t h[512] = {0};
    std::memcpy(h, name.data(), std::min<size_t>(name.size(), 100));
    std::memcpy(h + 100, "0000644", 7);
    std::memcpy(h + 108, "0000000", 7);
    std::memcpy(h + 116, "0000000", 7);
    std::snprintf(reinterpret_cast<char*>(h + 124), 12, "%011llo",
                  static_cast<unsigned long long>(size) & 077777777777ULL);  // 11 octal digits, as tar allows
    std::memcpy(h + 136, "00000000000", 11);
    h[156] = static_cast<uint8_t>(type);
    std::memcpy(h + 257, "ustar", 6);
    std::memcpy(h + 263, "00", 2);
    std::memset(h + 148, ' ', 8);
    unsigned sum = 0;
    for (uint8_t c : h) sum += c;
    std::snprintf(reinterpret_cast<char*>(h + 148), 8, "%06o", sum);
    out.insert(out.end(), h, h + 512);
}

void tar_member(Bytes& out, const std::string& name, const Bytes& data, char type = '0') {
    tar_header(out, name, data.size(), type);
    append(out, data);
    out.resize(out.size() + (512 - data.size() % 512) % 512, 0);
}

Bytes text(const std::string& s) { return Bytes(s.begin(), s.end()); }

struct Expanded {
    std::vector<ArchiveMember> members;
    std::vector<ScanFailure> failures;
    bool threw = false;
};

Expanded expand(const Bytes& archive, const std::string& name, const ArchiveLimits& limits = ArchiveLimits()) {
    Expanded e;
    try {
        e.members = expand_archive(name, archive.data(), archive.size(), limits, nullptr, e.failures);
    } catch (...) {
        e.threw = true;
    }
    return e;
}

size_t count_with(const std::vector<ScanFailure>& failures, const char* text) {
    size_t n = 0;
    for (const ScanFailure& f : failures) n += f.reason.find(text) != std::string::npos;
    return n;
}

size_t count_of(const std::string& s, const std::string& part) {
    size_t n = 0;
    for (size_t at = s.find(part); at != std::string::npos; at = s.find(part, at + part.size())) ++n;
    return n;
}

}  // namespace

TEST(archive_repeated_local_headers_are_read_once) {
    // One member of 2 MiB of zeros (about 13 KB deflated) and 3000 central directory entries that all point at it:
    // expanded as written, the file would be decompressed 3000 times.
    Zip zip;
    const Bytes zeros = deflate_zeros(2u << 20);
    const uint32_t at = zip.local("zeros.bin", zeros, 8);
    for (int i = 0; i < 3000; ++i)
        zip.entry("copy" + std::to_string(i) + ".bin", at, static_cast<uint32_t>(zeros.size()), 8);
    const Expanded e = expand(zip.bytes(), "same.zip");
    CHECK(!e.threw);
    CHECK(e.members.size() == 1 && e.members[0].bytes.size() == (2u << 20));
    CHECK(count_with(e.failures, "overlaps another member") == pb::detect::kArchiveMaxFailures);
    CHECK(e.failures.size() == pb::detect::kArchiveMaxFailures + 1);
    CHECK(!e.failures.empty() && e.failures.back().file == "same.zip" &&
          e.failures.back().reason.find("1999 more failures") == 0);

    // An entry whose local header lies inside another member's data (overlapping without being equal) is refused
    // too: the outer member's stored data quotes a whole local entry.
    const Bytes inner_data = deflate_stored(text("inner member"));
    Bytes payload = text("pad:");  // so that the outer member does not itself start like a zip
    append(payload, local_entry("inside.txt", inner_data, 8));
    const Bytes quoted = deflate_stored(payload);
    Zip nested;
    const uint32_t outer_at = nested.local("outer.txt", quoted, 8);
    nested.entry("outer.txt", outer_at, static_cast<uint32_t>(quoted.size()), 8);
    // After the outer local header (30 bytes and its name) and the stored block's 5-byte header: "pad:", then it.
    nested.entry("inside.txt", outer_at + 30 + 9 + 5 + 4, static_cast<uint32_t>(inner_data.size()), 8);
    const Expanded n = expand(nested.bytes(), "nested.zip");
    CHECK(n.members.size() == 1 && n.members[0].name == "nested.zip!/outer.txt");
    CHECK(n.failures.size() == 1 && count_with(n.failures, "overlaps another member") == 1);
}

TEST(archive_failed_members_are_charged_to_the_budget) {
    // 64 distinct members, each 128 KiB of zeros, with a member limit of 64 KiB: each one fails. Their decompressed
    // bytes still count: the 1 MiB budget stops the archive after 16 of them.
    Zip zip;
    for (int i = 0; i < 64; ++i) zip.add("bomb" + std::to_string(i) + ".bin", deflate_zeros(128u << 10), 8);
    ArchiveLimits limits;
    limits.max_member_bytes = 64u << 10;
    limits.max_total_bytes = 1u << 20;
    const Expanded e = expand(zip.bytes(), "bombs.zip", limits);
    CHECK(!e.threw && e.members.empty());
    CHECK(count_with(e.failures, "decompressed size over the limit") == 16);
    CHECK(count_with(e.failures, "bytes decompressed from one archive") == 1);
    CHECK(e.failures.size() == 17);

    // The same members within the limits are all extracted: the budget only stops what is over it.
    limits.max_member_bytes = 256u << 10;
    limits.max_total_bytes = 64u << 20;
    const Expanded ok = expand(zip.bytes(), "bombs.zip", limits);
    CHECK(ok.members.size() == 64 && ok.failures.empty());
}

TEST(archive_long_names_are_cut_in_the_middle) {
    const size_t cap = pb::detect::kArchiveMaxNameBytes;
    const std::string long_name = "deep/" + std::string(60000, 'd') + "/settings.properties";

    // zip: a 60 KB entry name.
    Zip zip;
    zip.add(long_name, text("key=value\n"), 0);
    const Expanded z = expand(zip.bytes(), "outer.zip");
    CHECK(z.members.size() == 1);
    if (!z.members.empty()) {
        const std::string& name = z.members[0].name;
        CHECK(name.size() <= std::string("outer.zip").size() + cap);
        CHECK(name.rfind("outer.zip!/deep/ddd", 0) == 0);
        CHECK(name.size() > 20 && name.compare(name.size() - 20, 20, "/settings.properties") == 0);
        CHECK(name.find("\xE2\x80\xA6") != std::string::npos);
    }

    // tar: a GNU long name of 100 KB, then a pax path of 70 KB.
    Bytes tar;
    const std::string gnu = std::string(100000, 'g') + "/a.env";
    tar_member(tar, "././@LongLink", text(gnu + '\0'), 'L');
    tar_member(tar, "short-a.env", text("A=1\n"));
    const std::string record_body = " path=" + std::string(70000, 'p') + "/b.env\n";
    const std::string record = std::to_string(record_body.size() + 5) + record_body;  // 5 digits, counted in the length
    tar_member(tar, "PaxHeader", text(record), 'x');
    tar_member(tar, "short-b.env", text("B=2\n"));
    tar.resize(tar.size() + 1024, 0);
    const Expanded t = expand(tar, "bundle.tar");
    CHECK(t.failures.empty() && t.members.size() == 2);
    for (const ArchiveMember& m : t.members) {
        CHECK(m.name.size() <= std::string("bundle.tar").size() + cap);
        CHECK(m.name.find("\xE2\x80\xA6") != std::string::npos);
    }
    if (t.members.size() == 2) {
        CHECK(t.members[0].name.size() > 6 &&
              t.members[0].name.compare(t.members[0].name.size() - 6, 6, "/a.env") == 0);
        CHECK(t.members[1].name.size() > 6 &&
              t.members[1].name.compare(t.members[1].name.size() - 6, 6, "/b.env") == 0);
    }

    // gzip: a 10 KB FNAME. And a 60 KB name inside a 60 KB name: nesting never makes a name longer.
    const Expanded g = expand(gzip(text("x=1\n"), std::string(10000, 'n') + ".txt"), "a.gz");
    CHECK(g.members.size() == 1 && g.members[0].name.size() <= 4 + cap);
    Zip inner;
    for (int i = 0; i < 3; ++i) inner.add(std::to_string(i) + std::string(60000, 'i') + ".cfg", text("v\n"), 0);
    Zip outer;
    outer.add(std::string(60000, 'o') + ".zip", inner.bytes(), 0);
    const Expanded nested = expand(outer.bytes(), "top.zip");
    CHECK(nested.members.size() == 3);
    for (const ArchiveMember& m : nested.members) {
        CHECK(m.name.size() <= std::string("top.zip").size() + cap);
        CHECK(m.name.size() > 4 && m.name.compare(m.name.size() - 4, 4, ".cfg") == 0);
    }

    // A name that fits is left as it is.
    Zip plain;
    plain.add("src/main/resources/application.yml", text("a: b\n"), 0);
    const Expanded p = expand(plain.bytes(), "app.jar");
    CHECK(p.members.size() == 1 && p.members[0].name == "app.jar!/src/main/resources/application.yml");
}

TEST(archive_failures_are_listed_within_a_limit) {
    // A readable member, then 3000 encrypted entries: 1000 are listed, then one failure counts the others.
    Zip zip;
    zip.add("readme.txt", text("hello\n"), 0);
    for (int i = 0; i < 3000; ++i) zip.entry("secret" + std::to_string(i) + ".txt", 0, 0, 8, 1);
    const Expanded e = expand(zip.bytes(), "locked.zip");
    CHECK(!e.threw && e.members.size() == 1);
    CHECK(count_with(e.failures, "encrypted") == pb::detect::kArchiveMaxFailures);
    CHECK(e.failures.size() == pb::detect::kArchiveMaxFailures + 1);
    CHECK(!e.failures.empty() && e.failures.back().file == "locked.zip" &&
          e.failures.back().reason.find("2000 more failures") == 0);

    // Names count against the budget: members with 4 KB names stop at the budget whatever their size.
    Zip names;
    for (int i = 0; i < 200; ++i) names.add(std::to_string(i) + std::string(4000, 'n') + ".txt", text("1"), 0);
    ArchiveLimits limits;
    limits.max_total_bytes = 64u << 10;
    const Expanded n = expand(names.bytes(), "names.zip", limits);
    CHECK(n.members.size() >= 15 && n.members.size() <= 17);
    CHECK(count_with(n.failures, "bytes decompressed from one archive") == 1);
}

TEST(archive_gzip_header_past_the_end_is_a_failure) {
    // 18 bytes: FEXTRA and FNAME set, and an extra field of 65535 bytes in a file of 18.
    const Bytes header = {0x1f, 0x8b, 8, 4 | 8, 0, 0, 0, 0, 0, 255, 0xFF, 0xFF, 'a', 'b', 'c', 0, 0, 0};
    const Expanded e = expand(header, "short.gz");
    CHECK(!e.threw && e.members.empty());
    CHECK(e.failures.size() == 1 && count_with(e.failures, "corrupt gzip header") == 1);

    // Through the C API: a status of PB_OK and the failure, not an error for the call.
    pb_archive* archive = nullptr;
    pb_error err;
    CHECK(pb_archive_expand(header.data(), header.size(), "short.gz", nullptr, &archive, &err) == PB_OK);
    CHECK(pb_archive_failure_count(archive) == 1);
    CHECK(archive && std::strstr(pb_archive_failure(archive, 0), "corrupt gzip header") != nullptr);
    pb_archive_free(archive);

    // Every other header flag at the end of the data: failures too.
    for (int flags : {2, 8, 16, 2 | 8 | 16, 4}) {
        Bytes cut = {0x1f, 0x8b, 8, static_cast<uint8_t>(flags), 0, 0, 0, 0, 0, 255};
        while (cut.size() < 18) cut.push_back(static_cast<uint8_t>(flags & 4 ? 0x40 : 'z'));
        const Expanded c = expand(cut, "cut.gz");
        CHECK(!c.threw && c.members.empty() && c.failures.size() == 1);
    }
}

TEST(archive_limits_from_the_api_are_clamped) {
    // 20 gzip files one inside the other, expanded with a depth limit of INT32_MAX: 16 levels at most.
    Bytes data = text("KEY=value\n");
    for (int level = 0; level < 20; ++level) data = gzip(data);
    pb_archive_limits limits;
    pb_archive_limits_init(&limits);
    limits.max_depth = INT32_MAX;
    pb_archive* archive = nullptr;
    pb_error err;
    CHECK(pb_archive_expand(data.data(), data.size(), "deep.gz", &limits, &archive, &err) == PB_OK);
    CHECK(pb_archive_member_count(archive) == 1);
    const char* name = pb_archive_member_name(archive, 0);
    CHECK(name && count_of(name, "!/") == static_cast<size_t>(pb::detect::kArchiveMaxDepth));
    CHECK(pb_archive_failure_count(archive) == 1 &&
          std::strstr(pb_archive_failure(archive, 0), "nested deeper than the limit") != nullptr);
    pb_archive_free(archive);

    // A ratio so large that compressed * ratio would wrap around to a small number: it means "no ratio limit".
    Zip zip;
    const Bytes zeros = deflate_zeros(3u << 20);
    zip.add("zeros.bin", zeros, 8);
    const Bytes file = zip.bytes();
    pb_archive_limits_init(&limits);
    limits.max_ratio = UINT64_MAX / zeros.size() + 1;
    CHECK(pb_archive_expand(file.data(), file.size(), "ratio.zip", &limits, &archive, &err) == PB_OK);
    size_t size = 0;
    CHECK(pb_archive_member_count(archive) == 1 && pb_archive_member_data(archive, 0, &size) && size == (3u << 20));
    CHECK(pb_archive_failure_count(archive) == 0);
    pb_archive_free(archive);

    // A small ratio still refuses the same member (3 MiB from about 20 KB: over 10 x 20 KB + 1 MiB).
    limits.max_ratio = 10;
    CHECK(pb_archive_expand(file.data(), file.size(), "ratio.zip", &limits, &archive, &err) == PB_OK);
    CHECK(pb_archive_member_count(archive) == 0 && pb_archive_failure_count(archive) == 1);
    pb_archive_free(archive);
}
