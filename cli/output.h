// Where reports go (standard output or --out FILE) and the documents `purebyte scan` writes (spec/OUTPUT.md).
#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "args.h"
#include "core/json.h"
#include "inputs.h"
#include "setup.h"

namespace cli {

class Output {
public:
    // `path` empty = standard output. Written in binary mode: the bytes are exactly the report's.
    explicit Output(const std::string& path);
    ~Output();
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;

    // A failed write (a full disk, a closed pipe) throws UsageError with exit status 2: a report or a copy that is
    // incomplete is never taken for a result.
    void write(const std::string& text);  // text and a newline, flushed
    void write_bytes(const uint8_t* data, size_t size);
    // Flushes (and closes a file); throws like write. Call it before reporting success.
    void close();

private:
    [[noreturn]] void fail() const;
    std::FILE* file_ = nullptr;
    bool owned_ = false;
    std::string name_;  // for messages
};

// Everything `purebyte scan` reports, gathered batch by batch.
struct Report {
    std::vector<pb::json::Value> results;   // one per analyzed input (json format)
    std::vector<pb::json::Value> findings;  // the flat list (json and sarif formats)
    std::vector<long long> columns;         // SARIF start column (code points) of each finding; 0 when it has no line
    std::vector<Failure> failures;
    int64_t files = 0, bytes = 0, findings_count = 0, files_with_findings = 0;
    int64_t errors = 0, warnings = 0;  // findings by severity
    double seconds = 0;
};

std::string render_scan_json(const Report& report, const Setup& setup, const Args& args);
std::string render_scan_sarif(const Report& report, const Setup& setup);
// JSON Lines: a line for an input that could not be scanned.
std::string render_failure_line(const Failure& failure);

// 1-based column of byte `start` in Unicode code points, as SARIF counts columns.
long long code_point_column(const std::vector<uint8_t>& bytes, long long start);

}  // namespace cli
