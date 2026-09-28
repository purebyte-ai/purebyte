// purebyte redact: a copy of the input with every detected span replaced by a typed marker; --restore undoes it.
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>

#include "catalog.h"
#include "commands.h"
#include "core/files.h"
#include "inputs.h"
#include "output.h"
#include "setup.h"

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cli {

namespace {

constexpr uint64_t kSpansInputLimit = 1ull << 30;  // --spans: no model, so no model limit; 1 GiB held in memory

// The options that choose and set up a model (setup.h), which --spans and --restore do not use.
const char* const kModelOptions[] = {"model", "ensemble",       "no-ensemble", "profile",       "bias",  "type-bias",
                                     "votes", "min-confidence", "threads",     "intra-threads", "kernel"};

const char* kUsage =
    "usage: purebyte redact --model NAME [options] [INPUT | -]\n"
    "       purebyte redact --spans SPANS.json [options] [INPUT | -]\n"
    "       purebyte redact --restore REDACTED --map MAP.json [--out FILE]\n"
    "\n"
    "Writes the input byte for byte, except that every detected span becomes a typed marker such as [EMAIL_1] (the\n"
    "same value always gets the same marker). Nothing outside the spans changes; this is checked before writing.\n"
    "--map writes the reversible mapping: it holds the original values, treat it as a secret.\n"
    "SPANS.json: [[start, end, \"TYPE\", confidence?, \"source\"?], ...] or [{\"start\", \"end\", \"type\", ...}, ...].\n\n";

// Writes all of `text` and closes `f`; throws UsageError (exit 2) when any of it fails.
void write_all(std::FILE* f, const std::string& path, const std::string& text) {
    errno = 0;
    const bool written = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    const int error = errno;
    if (std::fclose(f) != 0 || !written)
        throw UsageError("cannot write " + display_path(path) + (error ? std::string(": ") + std::strerror(error) : ""),
                         2);
}

// The map holds the redacted values. On Linux and macOS it is readable by its owner only, also when it already
// existed with other permissions, and a symbolic link in its place is refused rather than followed. On Windows it
// gets the permissions of its folder.
void write_private(const std::string& path, const std::string& text) {
#ifndef _WIN32
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
        throw UsageError("cannot write " + display_path(path) + ": " +
                             (errno == ELOOP ? "it is a symbolic link, which is not followed" : std::strerror(errno)),
                         2);
    struct stat st;
    if (::fstat(fd, &st) != 0 || (S_ISREG(st.st_mode) && ::fchmod(fd, 0600) != 0)) {
        const int error = errno;
        ::close(fd);
        throw UsageError("cannot make " + display_path(path) + " private: " + std::strerror(error), 2);
    }
    std::FILE* f = fdopen(fd, "wb");
    if (!f) ::close(fd);
#else
    std::FILE* f = pb::open_file(path, "wb");
#endif
    if (!f) throw UsageError("cannot write " + display_path(path) + ": " + std::strerror(errno), 2);
    write_all(f, path, text);
}

void write_file(const std::string& path, const std::string& text) {
    std::FILE* f = pb::open_file(path, "wb");
    if (!f) throw UsageError("cannot write " + display_path(path) + ": " + std::strerror(errno), 2);
    write_all(f, path, text);
}

// [[start, end, "TYPE", confidence?, "source"?], ...] or [{"start", "end", "type", "confidence"?, "source"?}, ...]
std::vector<pb_redaction_span> read_spans(const std::string& path, std::vector<std::string>& storage) {
    const std::vector<uint8_t> bytes = read_whole_file(path, "the spans file");
    pb::json::Value v;
    std::string error;
    if (!pb::json::parse(std::string(bytes.begin(), bytes.end()), v, error) || !v.is(pb::json::Value::Type::Array))
        throw UsageError(display_path(path) + " is not a JSON list of spans" + (error.empty() ? "" : ": " + error), 2);
    using Type = pb::json::Value::Type;
    // Every string must outlive the spans that point to it: reserve so that `storage` never reallocates.
    storage.reserve(v.array.size() * 2);
    std::vector<pb_redaction_span> spans;
    for (const pb::json::Value& s : v.array) {
        const bool list = s.is(Type::Array) && s.array.size() >= 3;
        const pb::json::Value* start = list ? &s.array[0] : s.get("start");
        const pb::json::Value* end = list ? &s.array[1] : s.get("end");
        const pb::json::Value* type = list ? &s.array[2] : s.get("type");
        const pb::json::Value* confidence = list ? (s.array.size() > 3 ? &s.array[3] : nullptr) : s.get("confidence");
        const pb::json::Value* source = list ? (s.array.size() > 4 ? &s.array[4] : nullptr) : s.get("source");
        if (!start || !end || !type || !start->is(Type::Number) || !end->is(Type::Number) || !type->is(Type::String) ||
            (confidence && !confidence->is(Type::Number)) || (source && !source->is(Type::String)))
            throw UsageError(display_path(path) + ": every span needs a numeric start and end and a type name", 2);
        pb_redaction_span span{static_cast<int64_t>(start->number), static_cast<int64_t>(end->number), nullptr,
                               confidence ? static_cast<float>(confidence->number) : 1.0f, nullptr};
        storage.push_back(type->string);
        span.type = storage.back().c_str();
        if (source) {
            storage.push_back(source->string);
            span.source = storage.back().c_str();
        }
        spans.push_back(span);
    }
    return spans;
}

}  // namespace

int command_redact(const std::vector<std::string>& argv) {
    std::vector<OptionSpec> spec = model_options();
    spec.insert(spec.end(),
                {
                    {"types", "T1,T2", "redact only these entity types"},
                    {"out", "FILE", "write the redacted copy to FILE (default: standard output)"},
                    {"report", "FILE", "write the report (types, offsets, markers; never the values) to FILE"},
                    {"map", "FILE", "write the reversible map to FILE (it holds the values)"},
                    {"spans", "FILE", "redact these spans (JSON) instead of running a model"},
                    {"restore", "FILE", "rebuild the original of a redacted FILE with --map"},
                    {"help", nullptr, "show this help"},
                });
    const Args a(argv, 2, spec);
    if (a.has("help")) {
        std::fputs((std::string(kUsage) + options_help(spec)).c_str(), stdout);
        return 0;
    }
    if (a.positional().size() > 1) throw UsageError("redact takes one input");
    const std::string source = a.positional().empty() ? "-" : a.positional()[0];
    // Every option given must apply: --restore uses only --map and --out, --spans no model option.
    if (a.has("restore") || a.has("spans")) {
        const char* mode = a.has("restore") ? "--restore" : "--spans";
        for (const char* option : kModelOptions)
            if (a.has(option)) throw UsageError(std::string(mode) + " runs no model: --" + option + " does not apply");
        if (a.has("types")) throw UsageError(std::string(mode) + " takes no --types: give the spans to redact");
    }
    pb_error err;
    if (a.has("restore")) {
        if (!a.has("map")) throw UsageError("--restore needs --map");
        if (a.has("spans") || a.has("report"))
            throw UsageError("--restore takes only --map and --out: --spans and --report do not apply");
        if (!a.positional().empty())
            throw UsageError("--restore reads the redacted FILE it names: give no other input");
        const std::vector<uint8_t> redacted = read_whole_file(a.text("restore"), "the redacted file");
        const std::vector<uint8_t> map = read_whole_file(a.text("map"), "the map");
        uint8_t* original = nullptr;
        size_t size = 0;
        check(pb_restore(redacted.data(), redacted.size(), std::string(map.begin(), map.end()).c_str(), &original,
                         &size, &err),
              err, "cannot restore");
        const std::unique_ptr<uint8_t, void (*)(void*)> owned(original, pb_free);
        Output out(a.text("out"));
        out.write_bytes(original, size);
        out.close();
        return 0;
    }
    pb_redaction* redaction = nullptr;
    const uint32_t flags = a.has("map") ? PB_REDACT_WITH_MAP : 0u;
    if (a.has("spans")) {
        std::vector<std::string> storage;
        const std::vector<pb_redaction_span> spans = read_spans(a.text("spans"), storage);
        const std::vector<uint8_t> input = read_one(source, kSpansInputLimit, false);
        check(pb_redact_spans(input.data(), input.size(), spans.data(), spans.size(), nullptr, 0, flags, &redaction,
                              &err),
              err, "cannot redact");
    } else {
        if (!a.has("model")) throw UsageError("redact needs --model (a model with typed spans) or --spans");
        const Setup s = setup(a, "");
        const std::vector<uint8_t> input = read_one(source, pb_detector_max_input_bytes(s.detector.get()));
        pb_redact_options o;
        pb_redact_options_init(&o);
        o.flags = flags;
        o.use_bias = s.settings.has_bias ? 1 : 0;
        o.bias = s.settings.bias;
        o.type_bias = s.settings.type_bias.empty() ? nullptr : s.settings.type_bias.data();
        o.type_bias_count = static_cast<uint32_t>(s.settings.type_bias.size());
        o.min_confidence = s.settings.min_confidence;
        const std::string types = a.text("types");
        o.types = types.empty() ? nullptr : types.c_str();
        const std::string name = source == "-" ? "<stdin>" : display_path(source);
        o.name = name.c_str();
        check(pb_redact(s.session.get(), s.detector.get(), input.data(), input.size(), &o, &redaction, &err), err,
              "cannot redact");
    }
    std::shared_ptr<pb_redaction> owned(redaction, pb_redaction_free);
    size_t size = 0;
    const uint8_t* bytes = pb_redaction_output(redaction, &size);
    const std::string report = pb_redaction_report(redaction);
    if (a.has("report")) write_file(a.text("report"), report + "\n");
    if (a.has("map")) write_private(a.text("map"), std::string(pb_redaction_map(redaction)) + "\n");
    Output out(a.text("out"));
    out.write_bytes(bytes, size);
    out.close();  // the copy is complete before it is called one
    pb::json::Value r;
    std::string error;
    const size_t count = pb::json::parse(report, r, error) && r.get("spans") ? r.get("spans")->array.size() : 0;
    std::fprintf(
        stderr, "purebyte: %zu span(s) redacted; outside them the output is identical to the input (checked)\n", count);
    return 0;
}

}  // namespace cli
