// The rules of the secrets-binary profile that need no model (detect/binary_rules.h): the metadata of signed JARs
// (digests and entry paths), which was most of the false alarms inside archives before this rule.
#include <string>
#include <vector>

#include "detect/binary_rules.h"
#include "test.h"

using pb::detect::is_jar_manifest_metadata;
using pb::detect::printable_string_around;

namespace {

// Whether the printable string holding the first occurrence of `needle` in `text` is taken for manifest metadata.
bool metadata_line(const std::string& text, const std::string& needle) {
    const std::vector<uint8_t> b(text.begin(), text.end());
    const size_t at = text.find(needle);
    CHECK(at != std::string::npos);
    const int64_t a = static_cast<int64_t>(at);
    return is_jar_manifest_metadata(b, printable_string_around(b, a, a + static_cast<int64_t>(needle.size())));
}

}  // namespace

TEST(binary_rules_jar_digest_attributes) {
    const std::string manifest =
        "Manifest-Version: 1.0\r\nCreated-By: 17 (Example)\r\n\r\n"
        "Name: org/example/Keys.class\r\nSHA-256-Digest: 3q2+7wAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\r\n\r\n";
    CHECK(metadata_line(manifest, "3q2+7w"));
    CHECK(metadata_line(manifest, "AAAA=\r"));  // a span that takes in the line break (seen in signed JARs)
    CHECK(!metadata_line(manifest, "Example"));
    const std::string signature_file =
        "Signature-Version: 1.0\n"
        "SHA-256-Digest-Manifest-Main-Attributes: abcdefghijklmnopqrstuvwxyzABCD\n"
        " EFGHIJKLMNOP=\n"
        "SHA1-Digest-Manifest: YWJjZGVmZ2hpamtsbW5vcHFyc3Q=\n";
    CHECK(metadata_line(signature_file, "abcdefghij"));
    CHECK(metadata_line(signature_file, "EFGHIJ"));  // the continuation line of the value above
    CHECK(metadata_line(signature_file, "YWJjZGVm"));
    CHECK(!metadata_line(signature_file, "Signature-Version"));
}

TEST(binary_rules_jar_entry_paths) {
    const std::string manifest =
        "Name: org/example/Keys.class\r\n"
        "Name: org/example/very/long/package/name/that/goes/past/seventy/two/Byt\r\n es$Inner.class\r\n";
    CHECK(metadata_line(manifest, "org/example/Keys.class"));
    CHECK(metadata_line(manifest, "package/name"));
    CHECK(metadata_line(manifest, "es$Inner"));  // the continuation of a long entry path
}

TEST(binary_rules_credentials_near_metadata_are_kept) {
    const std::string text =
        "Api-Key: 0123456789abcdef0123456789abcdef\n"
        "SHA-256-Digest: 3q2+7wAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\n"
        " password=hunter2-example\n"
        "config SHA-256-Digest: AAAAAAAAAAAAAAAA\n"
        "SHA-256-Digest-Token: AAAAAAAAAAAAAAAA\n"
        "SHA-256-Digest: not base64 at all\n"
        "SHA-256-Digest: 3q2+7wAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\r\n"
        "Name: org/example/Keys.class\r\n";
    CHECK(!metadata_line(text, "0123456789abcdef"));  // any other attribute is scanned as usual
    CHECK(!metadata_line(text, "password"));          // after a digest line, but no continuation of it
    CHECK(!metadata_line(text, "config"));            // the attribute name holds a space
    CHECK(!metadata_line(text, "SHA-256-Digest-Token"));
    CHECK(!metadata_line(text, "not base64"));
    CHECK(!metadata_line(text, "AAAA=\r\nName"));  // a span over two lines is judged whole: kept
    const std::string names = "Name: Z2hvc3Qta2V5LXZhbHVl\nName: key=0123456789abcdef\n";
    CHECK(!metadata_line(names, "Z2hvc3Qta2V5"));  // a "Name:" value that is no path
    CHECK(!metadata_line(names, "key=0123"));
}
