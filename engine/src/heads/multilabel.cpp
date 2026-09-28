// Head type `multilabel`: independent yes/no decisions for n labels on the window (a sigmoid per output of the
// pooled MLP, divided by the head's temperature). Label k is on when its probability reaches its threshold
// (`purebyte.head.<i>.thresholds`, default 0.5 each). As a gate it is positive when any label is on.
#include <cmath>
#include <vector>

#include "core/failure.h"
#include "heads/head.h"
#include "heads/pooling.h"

namespace pb {

namespace {

class MultiLabelHead final : public Head {
public:
    MultiLabelHead(LoadContext& ctx, int index) : Head(ctx, index, "multilabel") {
        d_ = ctx.d_model();
        pooling_ = read_pooling(file_, index);
        mlp_.load(ctx, "heads." + std::to_string(index) + ".", pooled_width(pooling_, d_));
        const std::string tkey = LoadContext::key("head", index, "temperature");
        temperature_ = checked_finite(file_, tkey, 1.0);
        if (!(temperature_ > 0.f)) fail(PB_ERR_FORMAT, "`" + tkey + "` must be a positive number");
        const std::vector<float> t = checked_finite_list(file_, LoadContext::key("head", index, "thresholds"));
        thresholds_.assign(static_cast<size_t>(mlp_.outputs()), 0.5f);
        if (!t.empty()) {
            if (static_cast<int>(t.size()) != mlp_.outputs())
                fail(PB_ERR_FORMAT,
                     format("head %d has %d labels and %zu thresholds", index, mlp_.outputs(), t.size()));
            thresholds_ = t;
        }
        if (!labels_.empty() && static_cast<int>(labels_.size()) != mlp_.outputs())
            fail(PB_ERR_FORMAT, format("head %d names %zu labels and has %d", index, labels_.size(), mlp_.outputs()));
        set_aggregate("max,mean,any", "max");
    }

    const char* type() const override { return "multilabel"; }
    bool can_gate() const override { return true; }
    bool positive(const HeadOutput& out) const override {
        if (!out.computed) return false;
        for (size_t k = 0; k < out.values.size(); ++k)
            if (out.values[k] >= thresholds_[k]) return true;
        return false;
    }
    std::vector<float> thresholds() const override { return thresholds_; }

    void run(const HeadInput& in, HeadOutput& out) const override {
        std::vector<float> pooled(static_cast<size_t>(pooled_width(pooling_, d_)));
        std::vector<float> hidden(static_cast<size_t>(mlp_.hidden()));
        out.values.assign(static_cast<size_t>(mlp_.outputs()), 0.f);
        pool(pooling_, in.hidden, in.T, d_, pooled.data());
        mlp_.run(pooled.data(), hidden.data(), out.values.data());
        for (float& v : out.values) v = 1.0f / (1.0f + std::exp(-(v / temperature_)));
        out.computed = true;
    }

    std::string describe() const override {
        json::Writer w = describe_begin();
        w.field("pooling", pooling_name(pooling_)).field("outputs", mlp_.outputs());
        w.field("temperature", static_cast<double>(temperature_)).key("thresholds").begin_array();
        for (float t : thresholds_) w.real(t);
        w.end_array().end_object();
        return w.take();
    }

private:
    Pooling pooling_;
    PooledMlp mlp_;
    int d_ = 0;
    float temperature_ = 1.f;
    std::vector<float> thresholds_;
};

}  // namespace

std::unique_ptr<Head> load_multilabel_head(LoadContext& ctx, int index) {
    return std::make_unique<MultiLabelHead>(ctx, index);
}

}  // namespace pb
