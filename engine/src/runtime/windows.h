// How an input is cut into windows.
#pragma once

#include <cstdint>
#include <vector>

namespace pb {

struct WindowSpan {
    int64_t start = 0;
    int32_t length = 0;
};

// Windows start at 0, stride, 2 * stride, ... and stop once one reaches the end of the input (spec/FORMAT.md, section
// 12.1). When the next window would be shorter than `min_length`, a last window of `window` bytes aligned to the end of
// the input is evaluated instead, so that every byte is in some window; an input shorter than `min_length` has no
// window. With a stride larger than the window the bytes between windows are never seen: callers keep
// stride <= window.
std::vector<WindowSpan> enumerate_windows(int64_t size, int window, int stride, int min_length);

// ceil((size - window) / stride) + 1 (1 when size <= window): the number of windows enumerate_windows gives an input
// of at least `min_length` bytes. Reported as the `total` of a progress indicator.
int64_t nominal_window_count(int64_t size, int window, int stride);

}  // namespace pb
