#include "runtime/prefilter.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "core/failure.h"

namespace pb {

namespace {

inline bool printable(uint8_t c) { return (c >= 0x20 && c < 0x7f) || c == '\t'; }
inline bool digit(char c) { return c >= '0' && c <= '9'; }

}  // namespace

Prefilter::Prefilter(const std::string& rule) {
    auto invalid = [&]() {
        fail(PB_ERR_ARGUMENT, "`" + rule + "` is not a prefilter rule (e.g. " + kDefaultRule + ")");
    };
    auto number = [&](const char*& q) {
        char* end = nullptr;
        const long v = std::strtol(q, &end, 10);
        q = end;
        return (v >= 1 && v <= 4096) ? static_cast<int>(v) : 0;
    };
    for (const char* q = rule.c_str(); *q;) {
        Component c{*q++, 0, 0};
        if (!std::strchr("AUPD", c.kind) || !digit(*q)) invalid();
        c.x = number(q);
        if (c.kind == 'P' || c.kind == 'D') {
            if (*q++ != 'x' || !digit(*q)) invalid();
            c.y = number(q);
            if (c.y == 0) invalid();
        }
        if (c.x == 0 || (c.kind == 'P' && c.x < 2) || (c.kind == 'D' && c.y > c.x)) invalid();
        components_.push_back(c);
        if (*q == '+')
            ++q;
        else if (*q)
            invalid();
    }
    if (components_.empty()) invalid();
}

bool Prefilter::keep(const uint8_t* d, int64_t n, int64_t s, int len) const {
    const int64_t e = s + len;
    for (const Component& c : components_) {
        if (c.kind == 'A') {
            // Scanning [s - L, e + L) is enough to see whether a run crossing an edge reaches L.
            const int64_t L = c.x, a = std::max<int64_t>(0, s - L), b = std::min(n, e + L);
            int64_t run = 0, run_start = a;
            for (int64_t k = a; k <= b; ++k) {
                if (k < b && printable(d[k])) {
                    if (run++ == 0) run_start = k;
                    continue;
                }
                if (run >= L && run_start < e && k > s) return true;  // the run [run_start, k) touches the window
                run = 0;
            }
        } else if (c.kind == 'U') {  // (printable, 0x00) pairs at offsets of parity p
            const int64_t L = c.x, a0 = std::max<int64_t>(0, s - 2 * L), b = std::min(n, e + 2 * L);
            for (int p = 0; p < 2; ++p) {
                int64_t run = 0, run_start = 0;
                for (int64_t k = a0 + (((a0 & 1) != p) ? 1 : 0); k <= b; k += 2) {
                    if (k + 1 < b && printable(d[k]) && d[k + 1] == 0) {
                        if (run++ == 0) run_start = k;
                        continue;
                    }
                    if (run >= L && run_start < e && k > s) return true;
                    run = 0;
                }
            }
        } else if (c.kind == 'P') {  // runs of b[i] == b[i+q]; the run [rs, i) covers the bytes [rs, i + q)
            const int64_t K = c.y;
            for (int64_t q = 2; q <= c.x; ++q) {
                const int64_t a = std::max<int64_t>(0, s - q - K), b = std::min(n - q, e + K);
                int64_t run = 0, run_start = a;
                bool varied = false;
                for (int64_t i = a; i <= b; ++i) {
                    if (i < b && d[i] == d[i + q]) {
                        if (run++ == 0) {
                            run_start = i;
                            varied = false;
                        }
                        if (d[i] != d[i + 1]) varied = true;  // not one repeated byte (zero or 0xFF padding)
                        continue;
                    }
                    if (run >= K && varied && run_start < e && i + q > s) return true;
                    run = 0;
                }
            }
        } else {  // D: slices [i, i + k), i in [s - k + 1, e), with >= m printable bytes
            const int64_t k = c.x, m = c.y;
            if (n < k) continue;
            const int64_t a = std::max<int64_t>(0, s - k + 1), b = std::min(n - k, e - 1);
            if (a > b) continue;
            int64_t count = 0;
            for (int64_t i = a; i < a + k; ++i) count += printable(d[i]);
            for (int64_t i = a;; ++i) {
                if (count >= m) return true;
                if (i + 1 > b) break;
                count += static_cast<int64_t>(printable(d[i + k])) - static_cast<int64_t>(printable(d[i]));
            }
        }
    }
    return false;
}

}  // namespace pb
