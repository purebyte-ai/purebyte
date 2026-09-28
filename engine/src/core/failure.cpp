#include "core/failure.h"

#include <cstdarg>
#include <cstdio>
#include <vector>

namespace pb {

std::string format(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    const int n = std::vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    std::string out;
    if (n > 0) {
        std::vector<char> buffer(static_cast<size_t>(n) + 1);
        std::vsnprintf(buffer.data(), buffer.size(), fmt, args);
        out.assign(buffer.data(), static_cast<size_t>(n));
    }
    va_end(args);
    return out;
}

void fail(pb_status status, const std::string& message) { throw Failure(status, message); }

}  // namespace pb
