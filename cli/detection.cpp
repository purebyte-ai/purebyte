#include "detection.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "args.h"
#include "engine.h"

namespace cli {

long long number_of(const pb::json::Value& v, const char* key, long long fallback) {
    const pb::json::Value* x = v.get(key);
    return x && x->is(pb::json::Value::Type::Number) ? static_cast<long long>(x->number) : fallback;
}

std::string string_of(const pb::json::Value& v, const char* key) {
    const pb::json::Value* x = v.get(key);
    return x && x->is(pb::json::Value::Type::String) ? x->string : std::string();
}

bool is_warning(const pb::json::Value& finding) { return string_of(finding, "severity") == "warning"; }

pb::json::Value detect(pb_session* session, pb_detector* detector, const std::vector<NamedInput>& inputs,
                       const DetectSettings& s, const std::vector<std::string>& paths) {
    std::vector<pb_input> in;
    std::vector<const char*> names, rule_paths;
    for (const NamedInput& i : inputs) {
        in.push_back({i.bytes.data(), i.bytes.size()});
        names.push_back(i.name.c_str());
    }
    for (const std::string& p : paths) rule_paths.push_back(p.empty() ? nullptr : p.c_str());
    if (!rule_paths.empty() && rule_paths.size() != inputs.size())
        throw UsageError("internal error: " + std::to_string(rule_paths.size()) + " paths for " +
                             std::to_string(inputs.size()) + " inputs",
                         2);
    std::vector<const char*> queries;
    for (const std::string& q : s.queries) queries.push_back(q.c_str());
    pb_detect_options o;
    pb_detect_options_init(&o);
    o.flags = (s.reveal ? PB_DETECT_REVEAL : 0u) | (s.per_model ? PB_DETECT_PER_MODEL : 0u) |
              (s.no_ensemble ? PB_DETECT_NO_ENSEMBLE : 0u) | (s.prefilter ? PB_DETECT_PREFILTER : 0u) |
              (s.strings_only ? PB_DETECT_STRINGS_ONLY : 0u) | (s.early_exit ? PB_DETECT_EARLY_EXIT : 0u) |
              (s.keep_examples ? PB_DETECT_KEEP_EXAMPLES : 0u) | (s.expand_archives ? PB_DETECT_EXPAND_ARCHIVES : 0u);
    o.votes = s.votes;
    o.use_bias = s.has_bias ? 1 : 0;
    o.bias = s.bias;
    o.type_bias = s.type_bias.empty() ? nullptr : s.type_bias.data();
    o.type_bias_count = static_cast<uint32_t>(s.type_bias.size());
    o.min_confidence = s.min_confidence;
    o.queries = queries.empty() ? nullptr : queries.data();
    o.query_count = static_cast<uint32_t>(queries.size());
    o.paths = rule_paths.empty() ? nullptr : rule_paths.data();
    char* json = nullptr;
    pb_error err;
    check(pb_detect(session, detector, in.data(), in.size(), names.data(), &o, &json, &err), err, "the scan failed");
    pb::json::Value out;
    std::string error;
    const bool parsed = pb::json::parse(json, out, error);
    pb_free(json);
    if (!parsed) throw UsageError("internal error: the library returned invalid JSON (" + error + ")", 2);
    return out;
}

ModelDeclaration declaration_of(const pb_model* model) {
    ModelDeclaration d;
    pb::json::Value v;
    std::string error;
    if (!pb::json::parse(pb_model_describe(model), v, error)) return d;
    d.profile = string_of(v, "profile");
    const pb::json::Value* heads = v.get("heads");
    if (!heads) return d;
    for (const pb::json::Value& h : heads->array) {
        if (string_of(h, "type") != "tag") continue;
        if (const pb::json::Value* labels = h.get("labels"))
            for (const pb::json::Value& l : labels->array) d.entities.push_back(l.string);
        const pb::json::Value* scalar = h.get("operating_bias");
        d.type_bias.assign(d.entities.size(), scalar ? static_cast<float>(scalar->number) : 0.f);
        if (const pb::json::Value* vec = h.get("operating_bias_vec"))
            for (size_t k = 0; k < vec->array.size() && k < d.type_bias.size(); ++k)
                d.type_bias[k] = static_cast<float>(vec->array[k].number);
        break;
    }
    return d;
}

std::vector<float> parse_type_bias(const std::string& spec, const ModelDeclaration& model) {
    std::vector<float> out = model.type_bias;
    for (const std::string& item : split_list(spec)) {
        const size_t eq = item.find('=');
        if (eq == std::string::npos) throw UsageError("`--type-bias` takes NAME=X items, not `" + item + "`");
        const std::string name = item.substr(0, eq);
        const auto it = std::find(model.entities.begin(), model.entities.end(), name);
        if (it == model.entities.end()) throw UsageError("`--type-bias`: the model has no entity type `" + name + "`");
        char* end = nullptr;
        const std::string value = item.substr(eq + 1);
        const double x = std::strtod(value.c_str(), &end);
        if (value.empty() || *end || !std::isfinite(x))
            throw UsageError("`--type-bias`: `" + value + "` is not a finite number");
        out[static_cast<size_t>(it - model.entities.begin())] = static_cast<float>(x);
    }
    return out;
}

}  // namespace cli
