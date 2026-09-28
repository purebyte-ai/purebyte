// Running a detector over named inputs through pb_detect, shared by scan, decide and the HTTP server.
#pragma once

#include <string>
#include <vector>

#include "core/json.h"
#include "purebyte/pb.h"

namespace cli {

// What the commands ask pb_detect for. Unlike the C API's flags, `strings_only` is on unless the user turns it off
// (--all-bytes, `strings_only=no`): a secrets-binary finding must touch a printable string. Other profiles ignore it.
struct DetectSettings {
    bool reveal = false;
    bool per_model = false;
    bool no_ensemble = false;
    bool prefilter = false;
    bool strings_only = true;
    bool early_exit = false;
    bool keep_examples = false;
    bool expand_archives = true;  // `serve` turns it off unless --archives is given
    int votes = 0;
    bool has_bias = false;
    float bias = 0.f;
    std::vector<float> type_bias;
    float min_confidence = 0.f;
    std::vector<std::string> queries;
};

struct NamedInput {
    std::string name;
    std::vector<uint8_t> bytes;
};

// pb_detect's document: {"profile", "models", "votes_needed", "results", "findings", "failures"} (spec/OUTPUT.md).
// `paths` (empty, or one per input; "" = its name): where each input is in the project scanned, for the profile's
// path rules (pb_detect_options.paths).
pb::json::Value detect(pb_session* session, pb_detector* detector, const std::vector<NamedInput>& inputs,
                       const DetectSettings& settings, const std::vector<std::string>& paths = {});

// A finding's severity ("error" or "warning"; spec/OUTPUT.md, section 5): "error" when it has none.
bool is_warning(const pb::json::Value& finding);

// What a model file declares that the CLI needs: its profile and its tag head's entity names and operating point.
struct ModelDeclaration {
    std::string profile;
    std::vector<std::string> entities;
    std::vector<float> type_bias;  // the operating bias of every entity type, as the model sets it
};
ModelDeclaration declaration_of(const pb_model* model);

// Parses "NAME=X,NAME=Y" into one bias per entity type; types not named keep the model's own bias.
std::vector<float> parse_type_bias(const std::string& spec, const ModelDeclaration& model);

// Member `key` of a JSON object as a number or a string, with a fallback when absent or of another type.
long long number_of(const pb::json::Value& v, const char* key, long long fallback = 0);
std::string string_of(const pb::json::Value& v, const char* key);

}  // namespace cli
