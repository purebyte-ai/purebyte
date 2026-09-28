// Profile `secrets-code`: credentials in source code, configuration and text files.
//
//   0. Walking a tree, it reads source, configuration and text files by name, skips dependency and build folders
//      (detect/code_files.h), and analyzes files of up to 4,000,000 bytes.
//   1. A file with a UTF-16 byte order mark is converted to UTF-8 (offsets then refer to the converted bytes).
//   2. Every window's spans are collected in order. A span is dropped when it lies inside a data URI payload, or
//      inside a readable base64 run (that run is scanned again, decoded, in step 4) unless the run decodes to
//      `user:password`, which is reported whole.
//   3. Once no later window can extend them (a span closes when the next window starts after it), spans at most 3
//      bytes apart are merged, split at lines that start a new assignment, and snapped to the quoted literal around
//      them or to the whole value of their assignment; a span touching a private-key block becomes the block. A
//      span overlapping an earlier finding of the same file is dropped, and so is one holding a well-known
//      documentation example key (unless asked to keep them), and one that starts on a line carrying the marker
//      `purebyte:allow` (the author's way to accept it, in a comment say).
//   4. Each readable base64 run of >= 24 decoded bytes is scanned as a document of its own; its spans are reported
//      at the run's position, as the line of decoded text around them (none when the run's line carries the marker).
//   5. With an ensemble, findings on the same line with overlapping columns are one finding, kept when a majority of
//      the members (or the requested number) report it.
// The order of these steps matters: it decides which of two overlapping candidates survives.
// Findings are errors, except in test, example and documentation paths (detect/code_files.h), where they are warnings:
// most of what the model finds there are credentials made for tests and examples.
#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>

#include "detect/code_files.h"
#include "detect/code_rules.h"
#include "detect/common.h"

namespace pb::detect {

namespace {

struct FileContext {
    std::vector<Base64Blob> blobs;
    std::vector<bool> blob_is_basic_auth;
    std::vector<Range> key_blocks, data_uris;
    std::unique_ptr<CodeIndex> code;
    std::vector<int64_t> allowed_lines;  // starts of the lines that carry the marker `purebyte:allow`, in order
};

// The lines that carry the marker `purebyte:allow` (it holds no line feed, so an occurrence lies on one line).
std::vector<int64_t> lines_with_marker(const std::vector<uint8_t>& b, const LineIndex& lines) {
    std::vector<int64_t> out;
    for (int64_t p = find_bytes(b, "purebyte:allow", 0); p >= 0; p = find_bytes(b, "purebyte:allow", p + 1)) {
        const int64_t start = lines.line_start(p);
        if (out.empty() || out.back() != start) out.push_back(start);
    }
    return out;
}

// True when the line that holds byte `at` carries the marker `purebyte:allow`: findings that start there are accepted
// by the author and not reported.
bool line_allows(const FileContext& x, const LineIndex& lines, int64_t at) {
    return std::binary_search(x.allowed_lines.begin(), x.allowed_lines.end(), lines.line_start(at));
}

// Steps 2 and 3 for one file and one ensemble member.
void first_pass(const std::vector<uint8_t>& b, const LineIndex& lines, const FileContext& x, const Model& model,
                const std::vector<WindowResult>& windows, int member, bool keep_examples, std::vector<Finding>& out,
                std::set<int64_t>& ignored_examples) {
    const int tag = model.find_head("tag");
    std::vector<Candidate> pending;
    // The ranges emitted so far: never empty, and disjoint (one overlapping an earlier one is not emitted), so the one
    // that starts last before a range's end is the only one that can overlap it.
    std::map<int64_t, int64_t> emitted;
    auto overlaps_emitted = [&](const Range& q) {
        auto e = emitted.lower_bound(q.second);
        return e != emitted.begin() && (--e)->second > q.first;
    };
    auto close = [&](int64_t until) {
        std::vector<Candidate> alive, ready;
        for (const Candidate& s : merge_nearby(pending, 3)) {
            if (s.end >= until) {
                alive.push_back(s);
                continue;
            }
            for (const Range& c : split_at_assignments(*x.code, s.start, s.end)) {
                const Range literal = snap_to_literal(*x.code, c.first, c.second);
                const Range r = literal != c ? literal : snap_to_value(*x.code, c.first, c.second);
                Candidate piece = s;
                piece.start = r.first;
                piece.end = r.second;
                ready.push_back(piece);
            }
        }
        pending.swap(alive);
        for (const Candidate& r : ready) {
            const Range q = extend_to_block(x.key_blocks, r.start, r.end);
            if (overlaps_emitted(q)) continue;
            emitted.emplace(q.first, q.second);
            if (!keep_examples && is_documentation_example(b, q.first, q.second)) {
                ignored_examples.insert(q.first);
                continue;
            }
            if (line_allows(x, lines, q.first)) continue;
            Finding f;
            f.start = q.first;
            f.end = q.second;
            f.type = r.type;
            f.confidence = r.confidence;
            f.model = member;
            locate(f, b, lines);
            out.push_back(std::move(f));
        }
    };
    for (const WindowResult& w : windows) {
        if (tag >= 0 && !w.heads.empty())
            for (const Span& s : w.heads[tag].spans) {
                bool skip = false;
                for (size_t i = 0; i < x.blobs.size() && !skip; ++i)  // at most 24 runs
                    skip = x.blobs[i].offset <= s.start && s.end <= x.blobs[i].offset + x.blobs[i].length &&
                           !x.blob_is_basic_auth[i];
                skip = skip || inside_one_of(x.data_uris, s.start, s.end);
                if (!skip) pending.push_back({s.start, s.end, s.type, s.confidence, "model"});
            }
        close(w.start);
    }
    close(static_cast<int64_t>(b.size()) + 1);
}

// Step 4 for one readable base64 run and one ensemble member: every span of every window of the decoded bytes is
// reported at the run's line and column, as the decoded line around it (no merge: the run is the finding's place).
void second_pass(const std::vector<uint8_t>& b, const LineIndex& lines, const FileContext& x, const Base64Blob& blob,
                 const Model& model, const std::vector<WindowResult>& windows, int member, std::vector<Finding>& out) {
    const int tag = model.find_head("tag");
    if (tag < 0 || line_allows(x, lines, blob.offset)) return;
    int64_t line, col, line_start, line_end;
    lines.locate(blob.offset, line, col, line_start, line_end);
    const Range context = context_range(b, blob.offset, blob.offset + blob.length, line_start, line_end);
    const std::string where = "(inside a base64) " + strip(text_of(b, context.first, context.second));
    for (const WindowResult& w : windows) {
        if (w.heads.empty()) continue;
        for (const Span& s : w.heads[tag].spans) {
            const Range t = line_around(blob.decoded, s.start, s.end);
            Finding f;
            f.line = line;
            f.col = col;
            f.end_line = line;
            f.start = blob.offset;
            f.end = blob.offset + blob.length;
            f.inside_base64 = true;
            f.inner_start = t.first;
            f.inner_end = t.second;
            f.text = text_of(blob.decoded, t.first, t.second, &f.text_code_points);
            f.multiline = f.text.find('\n') != std::string::npos;
            f.context = where;
            f.type = s.type;
            f.confidence = s.confidence;
            f.model = member;
            out.push_back(std::move(f));
        }
    }
}

class SecretsCodeProfile final : public Profile {
public:
    const char* name() const override { return "secrets-code"; }
    bool wants_file(const std::string& file_name) const override { return is_source_text_name(file_name); }
    bool wants_directory(const std::string& directory_name) const override {
        return !is_dependency_directory(directory_name);
    }
    uint64_t max_input_bytes() const override { return kTextMaxBytes; }
    Severity path_severity(const std::string& path) const override {
        return is_test_example_or_doc_path(path) ? Severity::warning : Severity::error;
    }

    std::vector<DocumentResult> run(Session& session, const DetectRequest& req,
                                    std::vector<Document>& docs) const override {
        const size_t F = docs.size();
        std::vector<DocumentResult> results(F);
        std::vector<FileContext> context(F);
        std::vector<LineIndex> lines;
        lines.reserve(F);
        for (size_t k = 0; k < F; ++k) {
            std::vector<uint8_t> converted;
            if (utf16_to_utf8(docs[k].bytes, converted)) {
                docs[k].bytes.swap(converted);
                results[k].counters["utf16_converted"] = 1;
            }
            lines.emplace_back(docs[k].bytes);  // reserved: the references below stay valid
            FileContext& x = context[k];
            x.code = std::make_unique<CodeIndex>(docs[k].bytes, lines.back());
            x.allowed_lines = lines_with_marker(docs[k].bytes, lines.back());
            x.blobs = readable_base64_blobs(docs[k].bytes);
            for (const Base64Blob& blob : x.blobs) x.blob_is_basic_auth.push_back(is_basic_auth(blob.decoded));
            x.key_blocks = private_key_blocks(docs[k].bytes);
            x.data_uris = data_uri_payloads(docs[k].bytes);
        }
        // The decoded base64 runs, scanned as documents of their own (in file order).
        std::vector<Document> inner;
        std::vector<std::pair<size_t, size_t>> inner_owner;  // (file, blob)
        for (size_t k = 0; k < F; ++k)
            for (size_t i = 0; i < context[k].blobs.size(); ++i)
                if (context[k].blobs[i].decoded.size() >= 24) {
                    inner.push_back({docs[k].name, context[k].blobs[i].decoded});
                    inner_owner.push_back({k, i});
                }

        const ScanOptions options = scan_options(req);
        const size_t M = req.models.size();
        std::vector<FindingsByMember> per(F, FindingsByMember(M));  // per document
        std::vector<std::set<int64_t>> ignored(F);
        for (size_t m = 0; m < M; ++m) {
            const Model& model = *req.models[m];
            const ScanResult scanned = scan_documents(session, model, docs, options);
            for (size_t k = 0; k < F; ++k) {
                // What the rules drop (spans in data URIs, documentation examples, allowed lines, spans overlapping an
                // earlier finding) is still masked in every view.
                add_hidden(results[k], window_spans(model, scanned.inputs[k]));
                first_pass(docs[k].bytes, lines[k], context[k], model, scanned.inputs[k], static_cast<int>(m),
                           req.keep_examples, per[k][m], ignored[k]);
                if (m == 0) summarize(model, scanned.inputs[k], results[k]);
            }
            if (inner.empty()) continue;
            const ScanResult inner_scanned = scan_documents(session, model, inner, options);
            for (size_t i = 0; i < inner.size(); ++i) {
                const size_t k = inner_owner[i].first;
                const Base64Blob& blob = context[k].blobs[inner_owner[i].second];
                if (!window_spans(model, inner_scanned.inputs[i]).empty())  // the run holds a marked value
                    add_hidden(results[k], {{blob.offset, blob.offset + blob.length, 0, 0.f, "model"}}, true);
                second_pass(docs[k].bytes, lines[k], context[k], blob, model, inner_scanned.inputs[i],
                            static_cast<int>(m), per[k][m]);
            }
        }
        for (size_t k = 0; k < F; ++k) {
            results[k].counters["ignored_documentation_examples"] = static_cast<int64_t>(ignored[k].size());
            conclude(results[k], docs[k], std::move(per[k]), req, vote_by_line);
        }
        return results;
    }
};

}  // namespace

const Profile& secrets_code_profile() {
    static const SecretsCodeProfile profile;
    return profile;
}

}  // namespace pb::detect
