#include "output.h"

#include <cerrno>
#include <cstring>

#include "catalog.h"
#include "core/files.h"
#include "core/utf8.h"
#include "sarif.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace cli {

Output::Output(const std::string& path) {
    if (path.empty()) {
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        file_ = stdout;
        name_ = "standard output";
        return;
    }
    name_ = display_path(path);
    file_ = pb::open_file(path, "wb");
    if (!file_) throw UsageError("cannot write " + name_ + ": " + std::strerror(errno), 2);
    owned_ = true;
}

Output::~Output() {  // after close(), or while an error is reported: nothing more can be said
    if (!file_) return;
    if (owned_)
        std::fclose(file_);
    else
        std::fflush(file_);
}

void Output::fail() const {
    const int error = errno;
    throw UsageError("cannot write " + name_ + (error ? std::string(": ") + std::strerror(error) : std::string()), 2);
}

void Output::write(const std::string& text) {
    errno = 0;
    if (std::fwrite(text.data(), 1, text.size(), file_) != text.size() || std::fputc('\n', file_) == EOF ||
        std::fflush(file_) != 0)
        fail();
}

void Output::write_bytes(const uint8_t* data, size_t size) {
    errno = 0;
    if (size && std::fwrite(data, 1, size, file_) != size) fail();
}

void Output::close() {
    if (!file_) return;
    errno = 0;
    std::FILE* f = file_;
    file_ = nullptr;
    const bool failed = owned_ ? std::fclose(f) != 0 : std::fflush(f) != 0 || std::ferror(f) != 0;
    if (failed) fail();
}

std::string render_scan_json(const Report& r, const Setup& s, const Args& a) {
    pb::json::Writer w;
    w.begin_object().field("purebyte", pb_version());
    w.key("model")
        .begin_object()
        .field("name", s.specialist.name)
        .field("version", s.specialist.version)
        .field("profile", s.profile);
    w.key("files").begin_array();
    for (const ModelFile& m : s.specialist.members)
        w.begin_object().field("path", m.path).field("sha256", m.sha256).end_object();
    w.end_array().end_object();
    const int members = s.settings.no_ensemble ? 1 : static_cast<int>(s.specialist.members.size());
    w.key("ensemble").begin_object().field("members", members);
    w.field("votes_needed", s.settings.votes > 0 ? s.settings.votes : members / 2 + 1).end_object();
    w.key("runtime")
        .begin_object()
        .field("kernel", pb_session_kernel(s.session.get()))
        .field("threads", s.threads)
        .end_object();
    if (a.has("staged")) w.key("scope").begin_object().field("staged", true).end_object();
    if (a.has("git-diff")) w.key("scope").begin_object().field("git_diff", a.text("git-diff")).end_object();
    w.key("results").begin_array();
    for (const pb::json::Value& v : r.results) w.raw(pb::json::dump(v));
    w.end_array().key("findings").begin_array();
    for (const pb::json::Value& v : r.findings) w.raw(pb::json::dump(v));
    w.end_array().key("failures").begin_array();
    for (const Failure& f : r.failures) w.begin_object().field("file", f.file).field("reason", f.reason).end_object();
    w.end_array();
    w.key("stats")
        .begin_object()
        .field("files", r.files)
        .field("bytes", r.bytes)
        .field("seconds", r.seconds)
        .field("findings", r.findings_count)
        .key("by_severity")
        .begin_object()
        .field("error", r.errors)
        .field("warning", r.warnings)
        .end_object()
        .field("files_with_findings", r.files_with_findings)
        .field("not_scanned", static_cast<int64_t>(r.failures.size()))
        .end_object();
    w.end_object();
    return w.take();
}

std::string render_scan_sarif(const Report& r, const Setup& s) {
    std::vector<SarifFinding> findings;
    for (size_t i = 0; i < r.findings.size(); ++i) findings.push_back({&r.findings[i], r.columns[i]});
    return render_sarif(findings, r.failures, s.specialist.name, s.specialist.version, s.settings.reveal);
}

std::string render_failure_line(const Failure& f) {
    pb::json::Writer w;
    w.begin_object().field("file", f.file).field("not_scanned", f.reason).end_object();
    return w.take();
}

long long code_point_column(const std::vector<uint8_t>& bytes, long long start) {
    if (start < 0 || static_cast<size_t>(start) > bytes.size()) return 0;
    long long line_start = start;
    while (line_start > 0 && bytes[static_cast<size_t>(line_start - 1)] != '\n') --line_start;
    size_t points = 0;
    pb::utf8::decode_replace(bytes.data() + line_start, static_cast<size_t>(start - line_start), &points);
    return static_cast<long long>(points) + 1;
}

}  // namespace cli
