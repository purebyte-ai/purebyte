#include "runtime/windows.h"

#include <algorithm>

namespace pb {

std::vector<WindowSpan> enumerate_windows(int64_t size, int window, int stride, int min_length) {
    std::vector<WindowSpan> out;
    for (int64_t start = 0;; start += stride) {
        const int64_t length = std::min<int64_t>(window, size - start);
        if (length < min_length) break;
        out.push_back({start, static_cast<int32_t>(length)});
        if (start + window >= size) return out;  // this window reaches the end of the input
    }
    // The next window would have been shorter than min_length while the last one did not reach the end: a window of
    // `window` bytes aligned to the end covers the bytes left (the last one was a whole window, so this one is too).
    if (!out.empty()) {
        const int64_t last = std::max<int64_t>(0, size - window);
        if (last > out.back().start) out.push_back({last, static_cast<int32_t>(size - last)});
    }
    return out;
}

int64_t nominal_window_count(int64_t size, int window, int stride) {
    return size <= window ? 1 : (size - window + stride - 1) / stride + 1;
}

}  // namespace pb
