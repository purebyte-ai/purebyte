// Head type `exit`: an early-exit probe on the residual stream after `layer` blocks. Features `rms_max_mean`: per
// position v = x / rms(x) (the RMSNorm operations without a weight), then [max over positions | mean over positions]
// (2 * d_model values; the mean summed in double and divided by T). score = bias + <w, features> in double. With
// early exit enabled, a window whose score is below `threshold` stops there and counts as negative.
//   heads.<i>.proj.weight [1, 2 * d_model], heads.<i>.proj.bias [1]
#include <cmath>

#include "core/failure.h"
#include "heads/head.h"

namespace pb {

namespace {

class RmsMaxMeanExit final : public ExitHead {
public:
    RmsMaxMeanExit(LoadContext& ctx, int index, int n_layers) : ExitHead(ctx, index, "exit") {
        d_ = ctx.d_model();
        const std::string p = "heads." + std::to_string(index) + ".";
        weight_ = ctx.f32(p + "proj.weight", {1, 2 * d_});
        bias_ = ctx.f32(p + "proj.bias", {1});
        // Every key of an exit head is required (spec/FORMAT.md, section 11.8): a missing one is a malformed file.
        const std::string features = file_.text(LoadContext::key("head", index, "features"));
        if (features != "rms_max_mean")
            fail(PB_ERR_UNSUPPORTED,
                 format("exit head %d: features `%s` (known: rms_max_mean)", index, features.c_str()));
        layer_ = static_cast<int>(checked_int(file_, LoadContext::key("head", index, "layer"), 1, n_layers - 1));
        threshold_ = checked_finite(file_, LoadContext::key("head", index, "threshold"));
    }

    const char* type() const override { return "exit"; }
    int layer() const override { return layer_; }
    float threshold() const override { return threshold_; }
    void run(const HeadInput&, HeadOutput&) const override {}
    size_t feature_floats() const override { return 2 * static_cast<size_t>(d_); }
    size_t sum_doubles() const override { return static_cast<size_t>(d_); }

    double score(const float* X, int T, float* f, double* sum) const override {
        for (int i = 0; i < d_; ++i) {
            f[i] = -INFINITY;
            sum[i] = 0.0;
        }
        for (int t = 0; t < T; ++t) {
            const float* x = X + static_cast<size_t>(t) * d_;
            double s = 0.0;
            for (int i = 0; i < d_; ++i) s += static_cast<double>(x[i]) * x[i];
            const float inv = 1.0f / std::sqrt(static_cast<float>(s / d_) + 1e-5f);
            for (int i = 0; i < d_; ++i) {
                const float v = x[i] * inv;
                f[i] = std::fmax(f[i], v);
                sum[i] += v;
            }
        }
        for (int i = 0; i < d_; ++i) f[d_ + i] = static_cast<float>(sum[i] / T);
        double z = bias_[0];
        for (int j = 0; j < 2 * d_; ++j) z += static_cast<double>(weight_[j]) * f[j];
        return z;
    }

    std::string describe() const override {
        json::Writer w = describe_begin();
        w.field("layer", layer_)
            .field("threshold", static_cast<double>(threshold_))
            .field("features", "rms_max_mean")
            .end_object();
        return w.take();
    }

private:
    int d_ = 0, layer_ = 0;
    float threshold_ = 0.f;
    const float* weight_ = nullptr;
    const float* bias_ = nullptr;
};

}  // namespace

std::unique_ptr<Head> load_exit_head(LoadContext& ctx, int index) {
    const int layers = static_cast<int>(ctx.file().integer("purebyte.n_layers"));
    return std::make_unique<RmsMaxMeanExit>(ctx, index, layers);
}

}  // namespace pb
