// A profile turns a model's raw window outputs over documents into results. The runtime knows nothing about what a
// span means; a profile does (credential format rules, redaction...). Profiles are listed in detect/registry.cpp: to
// add one, implement this interface in a new file and add one line there.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "detect/document.h"
#include "heads/head.h"
#include "model/model.h"
#include "runtime/session.h"

namespace pb::detect {

struct DetectRequest {
    std::vector<const Model*> models;  // models[0] is the main one; the others are ensemble members
    int votes = 0;                     // members that must agree; 0 = a majority
    SpanBias bias;
    float min_confidence = 0.f;
    bool per_model = false;      // keep every member's own findings
    bool keep_examples = false;  // secrets-code: keep well-known documentation example keys
    bool prefilter = false;
    bool strings_only = false;  // secrets-binary
    bool early_exit = false;
    std::vector<std::string> queries;  // query-conditioned models
    // > 0: the shortest window evaluated, instead of the model's `purebyte.window.min`. pb_redact sets 1: an input it
    // did not analyze would be copied out unredacted.
    int min_length = 0;
};

class Profile {
public:
    virtual ~Profile() = default;
    virtual const char* name() const = 0;
    // One result per document, in order. A profile may rewrite a document's bytes (its offsets then refer to them).
    virtual std::vector<DocumentResult> run(Session& session, const DetectRequest& request,
                                            std::vector<Document>& documents) const = 0;

    // What the profile reads when a caller walks directories or expands archives: files and directories by name
    // (a file named explicitly is always read), and the largest input it analyzes.
    virtual bool wants_file(const std::string& file_name) const {
        (void)file_name;
        return true;
    }
    virtual bool wants_directory(const std::string& directory_name) const { return directory_name != ".git"; }
    virtual uint64_t max_input_bytes() const { return uint64_t(64) << 20; }

    // The severity of the findings of a document, from its path inside the project scanned (e.g. relative to the
    // repository root, `/` or `\` as separators). Errors, unless the profile says otherwise.
    virtual Severity path_severity(const std::string& path) const {
        (void)path;
        return Severity::error;
    }
};

const Profile* find_profile(const std::string& name);
std::string known_profiles();

// Built-in profiles.
const Profile& none_profile();
const Profile& redact_profile();
const Profile& secrets_code_profile();
const Profile& secrets_binary_profile();

}  // namespace pb::detect
