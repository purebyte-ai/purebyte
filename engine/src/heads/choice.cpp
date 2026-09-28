// Head type `choice`: one decision among n classes for the window (softmax over the pooled MLP's logits divided by
// the head's temperature). Class 0 is the negative class: a choice head gates others with "argmax != 0".
#include <cmath>
#include <vector>

#include "core/failure.h"
#include "core/json.h"
#include "heads/head.h"
#include "heads/pooling.h"

namespace pb {

namespace {

class ChoiceHead final : public Head {
public:
    ChoiceHead(LoadContext& ctx, int index) : Head(ctx, index, "choice") {
        pooling_ = read_pooling(file_, index);
        mlp_.load(ctx, "heads." + std::to_string(index) + ".", pooled_width(pooling_, ctx.d_model()));
        d_ = ctx.d_model();
        const std::string key = LoadContext::key("head", index, "temperature");
        temperature_ = checked_finite(file_, key, 1.0);
        if (!(temperature_ > 0.f)) fail(PB_ERR_FORMAT, "`" + key + "` must be a positive number");
        if (!labels_.empty() && static_cast<int>(labels_.size()) != mlp_.outputs())
            fail(PB_ERR_FORMAT, format("head %d names %zu classes and has %d", index, labels_.size(), mlp_.outputs()));
        set_aggregate("max,mean,vote,any", "max");
    }

    const char* type() const override { return "choice"; }
    bool can_gate() const override { return true; }
    bool positive(const HeadOutput& out) const override { return out.computed && out.label != 0; }

    void run(const HeadInput& in, HeadOutput& out) const override {
        std::vector<float> pooled(static_cast<size_t>(pooled_width(pooling_, d_)));
        std::vector<float> hidden(static_cast<size_t>(mlp_.hidden()));
        out.values.assign(static_cast<size_t>(mlp_.outputs()), 0.f);
        pool(pooling_, in.hidden, in.T, d_, pooled.data());
        mlp_.run(pooled.data(), hidden.data(), out.values.data());
        float top = -1e30f;
        for (float& v : out.values) {
            v = v / temperature_;
            top = std::fmax(top, v);
        }
        float sum = 0.f;
        for (float& v : out.values) {
            v = std::exp(v - top);
            sum += v;
        }
        for (float& v : out.values) v /= sum;
        out.label = 0;
        for (int c = 1; c < mlp_.outputs(); ++c)
            if (out.values[c] > out.values[out.label]) out.label = c;
        out.computed = true;
    }

    std::string describe() const override {
        json::Writer w = describe_begin();
        w.field("pooling", pooling_name(pooling_))
            .field("classes", mlp_.outputs())
            .field("temperature", static_cast<double>(temperature_))
            .end_object();
        return w.take();
    }

private:
    Pooling pooling_;
    PooledMlp mlp_;
    int d_ = 0;
    float temperature_ = 1.f;
};

}  // namespace

std::unique_ptr<Head> load_choice_head(LoadContext& ctx, int index) { return std::make_unique<ChoiceHead>(ctx, index); }

}  // namespace pb
