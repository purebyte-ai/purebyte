// Error handling inside the library. Internal code throws `Failure`; the C API (api/) catches every exception at the
// boundary and turns it into a pb_status + message, so nothing ever escapes to the caller or aborts the process.
#pragma once

#include <exception>
#include <string>

#include "purebyte/pb.h"

namespace pb {

class Failure : public std::exception {
public:
    Failure(pb_status status, std::string message) : status_(status), message_(std::move(message)) {}
    pb_status status() const noexcept { return status_; }
    const char* what() const noexcept override { return message_.c_str(); }

private:
    pb_status status_;
    std::string message_;
};

// printf-style formatting into a std::string.
std::string format(const char* fmt, ...)
#if defined(__MINGW32__)
    __attribute__((format(gnu_printf, 1, 2)))
#elif defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

[[noreturn]] void fail(pb_status status, const std::string& message);

}  // namespace pb
