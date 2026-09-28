// Head type `score`: n real values for the window (regression): the pooled MLP's outputs, optionally mapped by an
// affine calibration value * scale + offset (`purebyte.head.<i>.scale`, `.offset`, one number per output).
#include <vector>

#include "core/failure.h"
#include "heads/head.h"
#include "heads/pooling.h"

namespace pb {

namespace {

class ScoreHead final : public Head {
public:
    ScoreHead(LoadContext& ctx, int index) : Head(ctx, index, "score") {
        d_ = ctx.d_model();
        pooling_ = read_pooling(file_, index);
        mlp_.load(ctx, "heads." + std::to_string(index) + ".", pooled_width(pooling_, d_));
        auto per_output = [&](const char* name, float fallback) {
            const std::vector<float> v = checked_finite_list(file_, LoadContext::key("head", index, name));
            std::vector<float> out(static_cast<size_t>(mlp_.outputs()), fallback);
            if (!v.empty()) {
                if (static_cast<int>(v.size()) != mlp_.outputs())
                    fail(PB_ERR_FORMAT,
                         format("head %d has %d outputs and %zu %s values", index, mlp_.outputs(), v.size(), name));
                out = v;
            }
            return out;
        };
        scale_ = per_output("scale", 1.f);
        offset_ = per_output("offset", 0.f);
        if (!labels_.empty() && static_cast<int>(labels_.size()) != mlp_.outputs())
            fail(PB_ERR_FORMAT, format("head %d names %zu outputs and has %d", index, labels_.size(), mlp_.outputs()));
        set_aggregate("mean,max,min", "mean");
    }

    const char* type() const override { return "score"; }

    void run(const HeadInput& in, HeadOutput& out) const override {
        std::vector<float> pooled(static_cast<size_t>(pooled_width(pooling_, d_)));
        std::vector<float> hidden(static_cast<size_t>(mlp_.hidden()));
        out.values.assign(static_cast<size_t>(mlp_.outputs()), 0.f);
        pool(pooling_, in.hidden, in.T, d_, pooled.data());
        mlp_.run(pooled.data(), hidden.data(), out.values.data());
        for (size_t k = 0; k < out.values.size(); ++k) out.values[k] = out.values[k] * scale_[k] + offset_[k];
        out.computed = true;
    }

    std::string describe() const override {
        json::Writer w = describe_begin();
        w.field("pooling", pooling_name(pooling_)).field("outputs", mlp_.outputs()).end_object();
        return w.take();
    }

private:
    Pooling pooling_;
    PooledMlp mlp_;
    int d_ = 0;
    std::vector<float> scale_, offset_;
};

}  // namespace

std::unique_ptr<Head> load_score_head(LoadContext& ctx, int index) { return std::make_unique<ScoreHead>(ctx, index); }

}  // namespace pb
