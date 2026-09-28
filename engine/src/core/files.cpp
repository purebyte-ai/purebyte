#include "core/files.h"

#include <cerrno>
#include <cstring>

#include "core/failure.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace pb {

std::FILE* open_file(const std::string& path, const char* mode) {
#ifdef _WIN32
    auto widen = [](const std::string& s) {
        const int n =
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), &w[0], n);
        return w;
    };
    const std::wstring wide_path = widen(path), wide_mode = widen(mode);
    if (wide_path.empty() && !path.empty()) {  // not valid UTF-8: no file can have this name
        errno = EILSEQ;
        return nullptr;
    }
    return _wfopen(wide_path.c_str(), wide_mode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

std::vector<uint8_t> read_file(const std::string& path) {
    errno = 0;
    std::FILE* f = open_file(path, "rb");
    if (!f) {
        const int error = errno;
        fail(PB_ERR_IO, format("cannot open %s: %s", path.c_str(),
                               error == EILSEQ ? "the path is not valid UTF-8"
                               : error         ? std::strerror(error)
                                               : "the C library gave no reason"));
    }
    std::vector<uint8_t> data;
    uint8_t chunk[1 << 16];
    for (;;) {
        const size_t got = std::fread(chunk, 1, sizeof chunk, f);
        data.insert(data.end(), chunk, chunk + got);
        if (got < sizeof chunk) break;
    }
    const bool failed = std::ferror(f) != 0;
    std::fclose(f);
    if (failed) fail(PB_ERR_IO, format("cannot read %s", path.c_str()));
    return data;
}

}  // namespace pb
