// Head type `ordinal`: a level among K ordered levels (CORAL): one shared linear score z of the pooled window and
// K-1 thresholds b sorted in decreasing order; P(level > k) = sigmoid(z + b_k) and the level is the number of those
// probabilities above 0.5. As a gate it is positive when the level is above 0.
//   heads.<i>.w.weight [1, pooled]  (no bias),  heads.<i>.b [K-1]
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include "core/failure.h"
#include "heads/head.h"
#include "heads/pooling.h"

namespace pb {

namespace {

class OrdinalHead final : public Head {
public:
    OrdinalHead(LoadContext& ctx, int index) : Head(ctx, index, "ordinal") {
        d_ = ctx.d_model();
        pooling_ = read_pooling(file_, index);
        const std::string p = "heads." + std::to_string(index) + ".";
        w_ = ctx.f32(p + "w.weight", {1, pooled_width(pooling_, d_)});
        const gguf::Tensor& b = ctx.tensor(p + "b");
        if (b.type != gguf::TensorType::F32 || b.dims.size() != 1 || b.count < 1)
            fail(PB_ERR_FORMAT, format("tensor `%sb` must be F32 [levels - 1]", p.c_str()));
        thresholds_.assign(b.f32(), b.f32() + b.count);
        std::sort(thresholds_.begin(), thresholds_.end(), std::greater<float>());
        if (!labels_.empty() && labels_.size() != thresholds_.size() + 1)
            fail(PB_ERR_FORMAT,
                 format("head %d names %zu levels and has %zu", index, labels_.size(), thresholds_.size() + 1));
        set_aggregate("max,mean", "max");
    }

    const char* type() const override { return "ordinal"; }
    bool can_gate() const override { return true; }
    bool positive(const HeadOutput& out) const override { return out.computed && out.label > 0; }

    void run(const HeadInput& in, HeadOutput& out) const override {
        const int width = pooled_width(pooling_, d_);
        std::vector<float> pooled(static_cast<size_t>(width));
        pool(pooling_, in.hidden, in.T, d_, pooled.data());
        float z = 0.f;
        for (int i = 0; i < width; ++i) z += w_[i] * pooled[i];
        out.values.resize(thresholds_.size());
        out.label = 0;
        for (size_t k = 0; k < thresholds_.size(); ++k) {
            out.values[k] = 1.0f / (1.0f + std::exp(-(z + thresholds_[k])));
            out.label += out.values[k] > 0.5f;
        }
        out.computed = true;
    }

    std::string describe() const override {
        json::Writer w = describe_begin();
        w.field("pooling", pooling_name(pooling_))
            .field("levels", static_cast<int>(thresholds_.size() + 1))
            .end_object();
        return w.take();
    }

private:
    Pooling pooling_;
    int d_ = 0;
    const float* w_ = nullptr;
    std::vector<float> thresholds_;  // sorted, largest first
};

}  // namespace

std::unique_ptr<Head> load_ordinal_head(LoadContext& ctx, int index) {
    return std::make_unique<OrdinalHead>(ctx, index);
}

}  // namespace pb
