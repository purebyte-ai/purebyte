// What the opaque handles of pb.h hold, and the guard that turns C++ exceptions into pb_status values at the C API
// boundary (nothing may escape to a C caller).
#pragma once

#include <algorithm>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "core/failure.h"
#include "core/utf8.h"
#include "model/model.h"
#include "purebyte/pb.h"
#include "runtime/scan.h"
#include "runtime/session.h"

struct pb_model {
    std::shared_ptr<const pb::Model> model;
};

struct pb_session {
    std::unique_ptr<pb::Session> session;
};

struct pb_scan_result {
    pb::ScanResult result;
    std::vector<std::vector<pb_window>> windows;  // per input
    std::vector<std::vector<pb_span>> spans;      // per input: the spans of the first tag head, window after window
};

namespace pb::api {

// Messages quote what files hold (key and tensor names, types, paths): they reach the caller as valid UTF-8 with
// every control character escaped, cut at a character boundary when they are longer than the buffer.
inline void set_error(pb_error* err, pb_status status, const char* message) noexcept {
    if (!err) return;
    err->status = status;
    try {
        const std::string clean = utf8::printable(message ? message : "");
        size_t n = std::min(clean.size(), sizeof err->message - 1);
        while (n > 0 && n < clean.size() && (static_cast<unsigned char>(clean[n]) & 0xC0) == 0x80) --n;
        std::memcpy(err->message, clean.data(), n);
        err->message[n] = '\0';
    } catch (...) {  // no memory for the copy: a message that needs none
        std::strncpy(err->message, "out of memory while reporting an error", sizeof err->message - 1);
        err->message[sizeof err->message - 1] = '\0';
    }
}

// Runs `body` and maps what it throws to a status (and message in `err`).
template <class Body>
pb_status guarded(pb_error* err, Body&& body) {
    try {
        body();
        if (err) {
            err->status = PB_OK;
            err->message[0] = '\0';
        }
        return PB_OK;
    } catch (const Failure& f) {
        set_error(err, f.status(), f.what());
        return f.status();
    } catch (const std::bad_alloc&) {
        set_error(err, PB_ERR_NO_MEMORY, "out of memory");
        return PB_ERR_NO_MEMORY;
    } catch (const std::exception& e) {
        set_error(err, PB_ERR_INTERNAL, e.what());
        return PB_ERR_INTERNAL;
    } catch (...) {
        set_error(err, PB_ERR_INTERNAL, "unknown internal error");
        return PB_ERR_INTERNAL;
    }
}

// Reads a caller's option struct over `out`, which holds the defaults. Option structs only grow by appending fields,
// so a caller built against an older header passes a smaller `struct_size` and keeps the defaults of the fields it
// does not know; `first_size` is the size of the struct in the first version of the ABI. A larger `struct_size` than
// this library's means the caller was built against a newer header: refused, since its new fields would be ignored.
template <class T>
void read_options(const T* in, T& out, size_t first_size, const char* name) {
    if (!in) return;
    const size_t size = in->struct_size;
    if (size < first_size)
        fail(PB_ERR_ARGUMENT,
             format("%s.struct_size is %zu, smaller than any version of it: initialise the struct with "
                    "%s_init",
                    name, size, name));
    if (size > sizeof(T))
        fail(PB_ERR_UNSUPPORTED, format("%s.struct_size is %zu and this library knows %zu bytes: the program was built "
                                        "against a newer purebyte/pb.h than the library it runs with",
                                        name, size, sizeof(T)));
    std::memcpy(static_cast<void*>(&out), in, size);
    out.struct_size = static_cast<uint32_t>(sizeof(T));
}

// Copies the user's operating point into the runtime's representation.
SpanBias span_bias(int32_t use_bias, float bias, const float* type_bias, uint32_t type_bias_count);
std::vector<std::string> string_list(const char* const* items, uint32_t count);

}  // namespace pb::api
