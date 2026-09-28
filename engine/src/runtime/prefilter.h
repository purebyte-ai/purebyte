// Window prefilter: skip windows that no byte REGION of interest touches, before running the model.
//
// A rule is a union of components written like "A16+U16":
//   A<L>      a maximal run of >= L printable ASCII bytes (0x20..0x7e or tab), what `strings` would print
//   U<L>      a maximal run of >= L UTF-16LE characters (printable byte + 0x00, at either parity)
//   P<p>x<K>  a periodic region: >= K consecutive i with b[i] == b[i+q] for some period q in 2..p, not one repeated
//             byte
//   D<k>x<m>  a k-byte slice holding >= m printable bytes
// Regions are measured in the whole input, not clipped to the window: one that only grazes a window still sends it to
// the model. The default, A16+U16, suits models whose positives live in strings; it is NOT exact for everything a
// model may flag (a model can also fire on compressed or tabular bytes outside strings, and those windows are skipped).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pb {

class Prefilter {
public:
    static constexpr const char* kDefaultRule = "A16+U16";

    // Throws Failure(PB_ERR_ARGUMENT) when `rule` is not a valid rule.
    explicit Prefilter(const std::string& rule);

    // True when the window [start, start + length) of data[0, size) must go through the model.
    bool keep(const uint8_t* data, int64_t size, int64_t start, int length) const;

private:
    struct Component {
        char kind;
        int x, y;
    };
    std::vector<Component> components_;
};

}  // namespace pb
