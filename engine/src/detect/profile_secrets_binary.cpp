// Profile `secrets-binary`: credentials inside binaries (executables, libraries, bytecode, archives' members).
//
//   1. Spans of every window at most 3 bytes apart are merged (optionally keeping only spans that touch a printable
//      string of >= 16 characters, ASCII or UTF-16LE).
//   2. Each merged span is reported with the printable ASCII string around it; a span inside the same string as the
//      previous finding and less than 512 bytes after it is the same finding (not listed again; like every span of
//      the model, it stays masked in the string shown). A span in the metadata of a JAR manifest or signature file
//      (digests "SHA-256-Digest: <base64>", entry paths "Name: org/...") is dropped: never a credential.
//   3. With an ensemble, a finding is its start offset, kept when a majority of the members (or the requested number)
//      report it.
// Every finding is an error, wherever the file is: a binary is what gets built and shipped, and a credential inside it
// ships with it, even from a test or example folder (secrets-code lowers those to warnings in source trees only).
#include "detect/binary_rules.h"
#include "detect/common.h"

namespace pb::detect {

namespace {

// Steps 1 and 2 for one file and one ensemble member, from the spans of its windows.
std::vector<Finding> findings_of(const std::vector<uint8_t>& b, const std::vector<Candidate>& window_spans, int member,
                                 bool strings_only) {
    std::vector<Candidate> spans;
    for (const Candidate& c : window_spans)
        if (!strings_only || touches_printable_string(b, c.start, c.end)) spans.push_back(c);
    // The spans come sorted and apart (merge_nearby): the string walks and the manifest checks are shared between the
    // spans of one string, so that a document of one long string costs O(n), not O(n) per span.
    std::vector<Finding> out;
    PrintableStrings strings(b);
    ManifestLines manifest(b);
    bool have_previous = false;
    Range previous{0, 0};
    int64_t previous_start = 0;
    for (const Candidate& s : merge_nearby(spans, 3)) {
        const Range around = strings.around(s.start, s.end);
        if (manifest.metadata(around)) continue;
        if (have_previous && s.start - previous_start < 512 && same_ascii_text(b, around, previous)) continue;
        have_previous = true;
        previous = around;
        previous_start = s.start;
        Finding f;
        f.binary = true;
        f.start = s.start;
        f.end = s.end;
        f.type = s.type;
        f.confidence = s.confidence;
        f.model = member;
        f.string_start = around.first;  // its text is read from the bytes when the caller reveals it
        f.string_end = around.second;
        f.text = ascii_text(b.data() + s.start, static_cast<size_t>(s.end - s.start));
        out.push_back(std::move(f));
    }
    return out;
}

class SecretsBinaryProfile final : public Profile {
public:
    const char* name() const override { return "secrets-binary"; }

    std::vector<DocumentResult> run(Session& session, const DetectRequest& req,
                                    std::vector<Document>& docs) const override {
        const size_t F = docs.size(), M = req.models.size();
        std::vector<DocumentResult> results(F);
        std::vector<FindingsByMember> per(F, FindingsByMember(M));  // per document
        const ScanOptions options = scan_options(req);
        for (size_t m = 0; m < M; ++m) {
            const Model& model = *req.models[m];
            const ScanResult scanned = scan_documents(session, model, docs, options);
            for (size_t k = 0; k < F; ++k) {
                if (m == 0) summarize(model, scanned.inputs[k], results[k]);
                // Every span is hidden in the views, the ones step 2 folds into an earlier finding of the same string
                // included: they are values the model found, printed in that finding's string otherwise.
                const std::vector<Candidate> spans = window_spans(model, scanned.inputs[k]);
                add_hidden(results[k], spans);
                per[k][m] = findings_of(docs[k].bytes, spans, static_cast<int>(m), req.strings_only);
            }
        }
        for (size_t k = 0; k < F; ++k) {
            results[k].binary = true;
            conclude(results[k], docs[k], std::move(per[k]), req, vote_by_offset);
        }
        return results;
    }
};

}  // namespace

const Profile& secrets_binary_profile() {
    static const SecretsBinaryProfile profile;
    return profile;
}

}  // namespace pb::detect
