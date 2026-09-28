// purebyte scan: files, directories, standard input, git changes and archives -> findings (JSON, JSONL or SARIF).
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "commands.h"
#include "inputs.h"
#include "output.h"
#include "setup.h"

namespace cli {

namespace {

constexpr int64_t kBatchBytes = 64ll << 20;  // inputs handed to the runtime at once

const char* kUsage =
    "usage: purebyte scan [options] [PATH ...]\n"
    "       purebyte scan [options] --staged [REPOSITORY] | --git-diff REF [REPOSITORY]\n"
    "\n"
    "Scans files and directories (walked recursively; symbolic links met on the way are not followed), standard input\n"
    "(`-`), the staged changes (--staged) or the changes since a git reference (--git-diff REF), and the members of\n"
    "zip, gzip and tar archives. With --staged and --git-diff, whole files are scanned and findings are kept on added\n"
    "lines only.\n"
    "Every finding has a severity: `error`, or `warning` when the profile lowers it by path (secrets-code: test,\n"
    "example and documentation paths such as tests/, src/test/, docs/, examples/, *_test.go, *.md).\n"
    "Exit status: 0 no error, 1 at least one error (with --strict: any finding), 2 something could not be scanned.\n\n";

std::vector<OptionSpec> scan_options() {
    std::vector<OptionSpec> spec = model_options();
    spec.insert(
        spec.end(),
        {
            {"format", "FORMAT", "json (default), jsonl (one line per input) or sarif"},
            {"out", "FILE", "write the report to FILE instead of standard output"},
            {"reveal", nullptr, "print detected values instead of masks (use with care)"},
            {"staged", nullptr, "scan the staged changes; report findings on added lines only"},
            {"git-diff", "REF", "scan the changes since REF; report findings on added lines only"},
            {"prefilter", nullptr, "skip windows without printable strings (faster on binaries; not exact)"},
            {"all-bytes", nullptr, "binaries: also report findings that touch no printable string"},
            {"early-exit", nullptr, "stop clearly negative windows early (models with an exit head; not exact)"},
            {"per-model", nullptr, "also report every ensemble member's own findings"},
            {"strict", nullptr, "exit 1 on warnings too (findings in test, example and documentation paths)"},
            {"tests", "yes|no", "no: skip test, example and documentation paths (their findings are warnings)"},
            {"exclude", "GLOB", "leave out the paths that match GLOB (repeatable; also read from .purebyteignore)",
             true},
            {"keep-examples", nullptr, "also report well-known documentation example keys"},
            {"no-archives", nullptr, "scan zip, gzip and tar files as they are instead of their members"},
            {"help", nullptr, "show this help"},
        });
    return spec;
}

// What `scan` reports, gathered as the batches come; JSON Lines are written as they come. With `drop_warnings`
// (--tests=no), findings that are warnings are left out: walks skip their files, but archive members and files named
// explicitly can still have some.
class Collector {
public:
    Collector(Output& out, const std::string& format, bool drop_warnings)
        : out_(out), format_(format), drop_warnings_(drop_warnings) {}

    void failure(const Failure& f) {
        if (format_ == "jsonl") out_.write(render_failure_line(f));
        report.failures.push_back(f);
    }

    // One analyzed input: its result and findings, minus the findings on lines the diff did not add.
    void result(pb::json::Value one, const std::vector<pb::json::Value>& findings, size_t& next_finding,
                const std::vector<uint8_t>& bytes, const AddedLines& added) {
        pb::json::Value* spans = nullptr;
        for (auto& kv : one.object)
            if (kv.first == "spans") spans = &kv.second;
        const pb::json::Value* counters = one.get("counters");
        const bool converted = counters && counters->get("utf16_converted");  // offsets refer to the converted text
        std::vector<pb::json::Value> kept_spans;
        pb::json::Value kept;
        kept.type = pb::json::Value::Type::Array;
        for (const pb::json::Value& span : spans->array) {  // a result's findings are its spans, in the same order
            const pb::json::Value& finding = findings.at(next_finding++);
            const long long line = number_of(finding, "line");
            if (line > 0 && !added.contains(line, number_of(finding, "end_line", line))) continue;
            const bool warning = is_warning(finding);
            if (warning && drop_warnings_) continue;
            (warning ? report.warnings : report.errors) += 1;
            kept_spans.push_back(span);
            kept.array.push_back(finding);
            if (format_ == "sarif")
                report.columns.push_back(line > 0 && !converted ? code_point_column(bytes, number_of(finding, "start"))
                                                                : 0);
        }
        spans->array = std::move(kept_spans);
        report.files += 1;
        report.bytes += number_of(one, "bytes");
        report.findings_count += static_cast<int64_t>(kept.array.size());
        report.files_with_findings += !kept.array.empty();
        if (format_ == "jsonl") {
            one.object.push_back({"findings", std::move(kept)});
            out_.write(pb::json::dump(one));
            return;
        }
        for (pb::json::Value& f : kept.array) report.findings.push_back(std::move(f));
        if (format_ == "json") report.results.push_back(std::move(one));
    }

    Report report;

private:
    Output& out_;
    const std::string& format_;
    const bool drop_warnings_;
};

// The engine's analysis of `inputs`, or false with the reason in `error` when it fails. For the test suite only (as
// git's GIT_TEST_* variables), PUREBYTE_TEST_FAIL_INPUT=NAME makes every call whose inputs include one named NAME fail
// as a failing engine would, to check that command_scan retries a failed batch input by input.
bool analyze(const Setup& s, const std::vector<NamedInput>& inputs, const std::vector<std::string>& paths,
             pb::json::Value& result, std::string& error) {
    try {
        const char* fail = std::getenv("PUREBYTE_TEST_FAIL_INPUT");
        for (const NamedInput& input : inputs)
            if (fail && *fail && input.name == fail)
                throw UsageError("the scan failed: PUREBYTE_TEST_FAIL_INPUT names this input");
        result = detect(s.session.get(), s.detector.get(), inputs, s.settings, paths);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

}  // namespace

int command_scan(const std::vector<std::string>& argv) {
    const std::vector<OptionSpec> spec = scan_options();
    const Args a(argv, 2, spec);
    if (a.has("help")) {
        std::fputs((std::string(kUsage) + options_help(spec)).c_str(), stdout);
        return 0;
    }
    const std::string format = a.text("format", "json");
    if (format != "json" && format != "jsonl" && format != "sarif")
        throw UsageError("--format takes json, jsonl or sarif");
    if (a.has("staged") && a.has("git-diff")) throw UsageError("--staged and --git-diff are alternatives: give one");
    const bool from_git = a.has("staged") || a.has("git-diff");
    if (from_git && a.positional().size() > 1)
        throw UsageError("--staged and --git-diff take at most one repository path");
    if (!from_git && a.positional().empty())
        throw UsageError("give files or directories to scan, `-` for standard input, or --staged");
    const std::string tests = a.text("tests", "yes");
    if (tests != "yes" && tests != "no") throw UsageError("--tests takes yes or no");
    const bool skip_tests = tests == "no", strict = a.has("strict");

    Setup s = setup(a, "secrets-code");
    s.settings.reveal = a.has("reveal");
    s.settings.per_model = a.has("per-model");
    s.settings.prefilter = a.has("prefilter");
    s.settings.strings_only = !a.has("all-bytes");
    s.settings.early_exit = a.has("early-exit");
    s.settings.keep_examples = a.has("keep-examples");
    s.settings.expand_archives = !a.has("no-archives");

    const std::string repository = a.positional().empty() ? "." : a.positional()[0];
    const pb_detector* detector = s.detector.get();
    const std::vector<std::string> excludes = a.all("exclude");
    const InputPlan plan = a.has("staged") ? plan_staged(repository, detector, skip_tests, excludes)
                           : a.has("git-diff")
                               ? plan_git_diff(a.text("git-diff"), repository, detector, skip_tests, excludes)
                               : plan_paths(a.positional(), detector, skip_tests, excludes);
    Output out(a.text("out"));
    Collector collect(out, format, skip_tests);
    for (const Failure& f : plan.failures) collect.failure(f);
    // What the engine made of some inputs: every result with its findings, then what it could not analyze.
    const auto add_results = [&collect](const pb::json::Value& result, const std::vector<NamedInput>& inputs,
                                        const std::vector<const Source*>& from) {
        size_t next_finding = 0;
        for (const pb::json::Value& one : result.get("results")->array) {
            const size_t k = static_cast<size_t>(number_of(one, "input"));
            collect.result(one, result.get("findings")->array, next_finding, inputs.at(k).bytes, from.at(k)->added);
        }
        for (const pb::json::Value& p : result.get("failures")->array)
            collect.failure({string_of(p, "file"), string_of(p, "reason")});
    };
    const auto started = std::chrono::steady_clock::now();
    for (size_t next = 0; next < plan.sources.size();) {
        std::vector<NamedInput> batch;
        std::vector<std::string> paths;  // for the profile's path rules
        std::vector<const Source*> from;
        int64_t bytes = 0;
        for (const size_t first = next; next < plan.sources.size() && (next == first || bytes < kBatchBytes); ++next) {
            const Source& src = plan.sources[next];
            NamedInput input{src.name, {}};
            if (!src.read(input.bytes)) {
                collect.failure({src.name, "could not be read"});
                continue;
            }
            bytes += static_cast<int64_t>(input.bytes.size());
            batch.push_back(std::move(input));
            paths.push_back(src.path);
            from.push_back(&src);
        }
        if (batch.empty()) continue;
        pb::json::Value result;
        std::string error;
        if (analyze(s, batch, paths, result, error)) {
            add_results(result, batch, from);
            continue;
        }
        // The engine failed on the batch: its inputs are analyzed one by one, so that an input the engine cannot
        // handle costs that input alone. One that fails on its own is reported as not scanned, and the scan goes on.
        if (batch.size() > 1)
            std::fprintf(stderr, "purebyte: %s; scanning these %zu inputs one by one\n", error.c_str(), batch.size());
        for (size_t k = 0; k < batch.size(); ++k) {
            std::vector<NamedInput> one;
            one.push_back(std::move(batch[k]));
            if (batch.size() > 1 && analyze(s, one, {paths[k]}, result, error))
                add_results(result, one, {from[k]});
            else
                collect.failure({one[0].name, error});
        }
    }
    Report& report = collect.report;
    report.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (format == "json") out.write(render_scan_json(report, s, a));
    if (format == "sarif") out.write(render_scan_sarif(report, s));
    out.close();
    std::fprintf(stderr,
                 "purebyte: %lld file(s), %.1f KiB, %lld finding(s) (%lld error(s), %lld warning(s)), %.1f s, %zu not "
                 "scanned\n",
                 static_cast<long long>(report.files), report.bytes / 1024.0,
                 static_cast<long long>(report.findings_count), static_cast<long long>(report.errors),
                 static_cast<long long>(report.warnings), report.seconds, report.failures.size());
    if (report.warnings && !strict)
        std::fprintf(stderr,
                     "purebyte: warnings (test, example or documentation paths) do not change the exit status; "
                     "--strict counts them\n");
    if (!report.failures.empty()) return 2;
    return report.errors || (strict && report.warnings) ? 1 : 0;
}

}  // namespace cli
