// A head turns the final hidden states of one window into outputs: class probabilities, scores, typed spans, labels
// per byte. Heads are listed in the file (`purebyte.head.<i>.type`) and built from the registry in heads/registry.cpp.
// To add a head type: implement this interface in a new file and add one line to that registry.
//
// Heads run on the team leader after the body (they are a small fraction of the work); `gated_by` makes a head run
// only when another head's decision is positive (e.g. spans only in windows the document head flags).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/json.h"
#include "kernels/kernels.h"
#include "model/load_context.h"

namespace pb {

// A span found by a tag head, relative to the window until the scan makes it absolute.
struct Span {
    int64_t start = 0;
    int64_t end = 0;       // exclusive
    int32_t type = 0;      // entity index
    float confidence = 0;  // mean probability of the chosen labels over the span's bytes
};

// The operating point of tag heads for one call: the user's choice, else the model's.
struct SpanBias {
    bool has_scalar = false;
    float scalar = 0.f;           // added to every non-O label
    std::vector<float> per_type;  // one per entity type; empty = not given
};

struct HeadInput {
    const kernels::Kernels& kernels;
    const float* hidden;  // [T, d_model]
    int T;
    int first_position;  // positions before it (a query prefix) produce no spans
    const SpanBias& bias;
};

struct HeadOutput {
    bool computed = false;
    int32_t label = -1;                // choice: argmax class; ordinal: level
    std::vector<float> values;         // choice/multilabel: probabilities; score: values; ordinal: P(level > k)
    std::vector<Span> spans;           // tag
    std::vector<uint8_t> byte_labels;  // byte_map: argmax class per position
};

class Head {
public:
    Head(LoadContext& ctx, int index, const char* type);
    virtual ~Head() = default;

    virtual const char* type() const = 0;
    int index() const { return index_; }
    const std::string& name() const { return name_; }
    int gated_by() const { return gated_by_; }
    // Names of the outputs: classes, entity types, score names or levels (may be empty).
    const std::vector<std::string>& labels() const { return labels_; }

    virtual void run(const HeadInput& in, HeadOutput& out) const = 0;
    // Heads that can gate others say whether their output is a positive decision.
    virtual bool can_gate() const { return false; }
    virtual bool positive(const HeadOutput&) const { return false; }
    // multilabel: the probability at which each label is on.
    virtual std::vector<float> thresholds() const { return {}; }
    // Document-level aggregation over windows ("max", "mean", "vote", "any"); empty for heads that do not aggregate.
    const std::string& aggregate() const { return aggregate_; }

    // JSON object describing the head (for pb_model_describe).
    virtual std::string describe() const = 0;

    void set_name(std::string name) { name_ = std::move(name); }

protected:
    // A JSON object opened with the fields every head has; describe() adds its own and closes it.
    json::Writer describe_begin() const;
    // Reads `purebyte.head.<i>.aggregate`, one of the comma-separated `allowed` values (default `fallback`).
    void set_aggregate(const std::string& allowed, const char* fallback);

    const gguf::File& file_;
    int index_;
    std::string name_;
    int gated_by_ = -1;
    std::vector<std::string> labels_;
    std::string aggregate_;
};

using HeadLoader = std::unique_ptr<Head> (*)(LoadContext& ctx, int index);

struct HeadType {
    const char* name;
    HeadLoader load;
};

const HeadType* find_head_type(const std::string& name);
std::string known_head_types();

// Loaders of the built-in head types.
std::unique_ptr<Head> load_choice_head(LoadContext& ctx, int index);
std::unique_ptr<Head> load_multilabel_head(LoadContext& ctx, int index);
std::unique_ptr<Head> load_score_head(LoadContext& ctx, int index);
std::unique_ptr<Head> load_ordinal_head(LoadContext& ctx, int index);
std::unique_ptr<Head> load_tag_head(LoadContext& ctx, int index);
std::unique_ptr<Head> load_byte_map_head(LoadContext& ctx, int index);
std::unique_ptr<Head> load_exit_head(LoadContext& ctx, int index);

// The early-exit head is evaluated INSIDE the body, after `layer` blocks: a window whose score is below the threshold
// stops there and counts as negative (heads not computed). It runs between the barriers of a team, so it must not
// allocate or throw: its scratch comes from the team's workspace.
class ExitHead : public Head {
public:
    using Head::Head;
    virtual int layer() const = 0;
    virtual float threshold() const = 0;
    // Scratch of score(): `features` floats and `sums` doubles.
    virtual size_t feature_floats() const = 0;
    virtual size_t sum_doubles() const = 0;
    // Score of the residual stream X [T, d_model] after `layer` blocks.
    virtual double score(const float* X, int T, float* features, double* sums) const = 0;
};

}  // namespace pb
