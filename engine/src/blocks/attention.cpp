// Block type `attention`: causal softmax self-attention with rotary position embeddings, and ternary or float
// projections (model/projection.h).
//
//   [q | k | v] = qkv(RMSNorm(x))              each d_model wide, heads of `head_dim` contiguous
//   q, k = RoPE(q), RoPE(k)                     rotate-half, base 10000, position = offset in the window
//   y[t] = sum_{j <= t} softmax_j(q[t] . k[j] / sqrt(head_dim)) v[j]
//   x = x + out(y)
//
// It needs the whole window (no carried state), so a model with an attention block runs in windows only.
#include <algorithm>
#include <cmath>
#include <vector>

#include "blocks/block.h"
#include "core/failure.h"
#include "core/json.h"
#include "model/projection.h"

namespace pb {

namespace {

class AttentionBlock final : public Block {
public:
    AttentionBlock(LoadContext& ctx, int index);

    const char* type() const override { return "attention"; }
    size_t scratch_floats(int T) const override { return static_cast<size_t>(T) * (6 * static_cast<size_t>(d_) + 1); }
    size_t member_scratch_floats(int T) const override { return 8 * static_cast<size_t>(d_) + static_cast<size_t>(T); }
    size_t position_floats() const override { return 3 * static_cast<size_t>(d_); }
    void forward(const BlockContext& ctx, float* X, int T, float* state) const override;
    std::string describe() const override;

private:
    void rope(float* v, int t) const;

    int d_ = 0, heads_ = 0, head_dim_ = 0;
    const float* norm_ = nullptr;
    std::vector<float> frequency_;  // RoPE frequency of each rotated pair
    Projection qkv_, out_;
};

AttentionBlock::AttentionBlock(LoadContext& ctx, int index) {
    const gguf::File& f = ctx.file();
    d_ = ctx.d_model();
    head_dim_ = static_cast<int>(checked_int(f, LoadContext::key("block", index, "head_dim"), 2, 4096, 64));
    heads_ = static_cast<int>(checked_int(f, LoadContext::key("block", index, "n_heads"), 1, 4096, d_ / head_dim_));
    if (heads_ * head_dim_ != d_ || head_dim_ % 2 != 0)
        fail(PB_ERR_FORMAT, format("attention block %d: %d heads of %d do not make d_model %d (head_dim must be even)",
                                   index, heads_, head_dim_, d_));
    const std::string p = "blocks." + std::to_string(index) + ".";
    norm_ = ctx.f32(p + "norm.weight", {d_});
    const int group = static_cast<int>(checked_int(f, "purebyte.tern.group", 1, 1 << 24, 0));  // 0: not declared
    qkv_ = Projection::load(ctx, p + "qkv.weight", 3 * d_, d_, group);
    out_ = Projection::load(ctx, p + "out.weight", d_, d_, group);
    const int half = head_dim_ / 2;
    const float step = static_cast<float>(-std::log(10000.0) / half);
    frequency_.resize(static_cast<size_t>(half));
    for (int j = 0; j < half; ++j) frequency_[j] = std::exp(static_cast<float>(j) * step);
}

// Rotate-half RoPE of one head vector at position t: (x1, x2) -> (x1 cos - x2 sin, x1 sin + x2 cos).
void AttentionBlock::rope(float* v, int t) const {
    const int half = head_dim_ / 2;
    for (int j = 0; j < half; ++j) {
        const float angle = static_cast<float>(t) * frequency_[j];
        const float c = std::cos(angle), s = std::sin(angle);
        const float x1 = v[j], x2 = v[j + half];
        v[j] = x1 * c - x2 * s;
        v[j + half] = x1 * s + x2 * c;
    }
}

void AttentionBlock::forward(const BlockContext& ctx, float* X, int T, float*) const {
    const kernels::Kernels& k = ctx.kernels;
    const Team& team = ctx.team;
    float* inv = ctx.scratch;
    float* Xn = inv + T;
    float* QKV = Xn + static_cast<size_t>(T) * d_;
    float* Y = QKV + static_cast<size_t>(T) * 3 * d_;
    float* Out = Y + static_cast<size_t>(T) * d_;
    float* fold = ctx.member_scratch;
    float* weights = ctx.member_scratch + 8 * static_cast<size_t>(d_);

    {
        const auto [t0, t1] = team.share(T, 16);
        k.rms_inv(X + static_cast<size_t>(t0) * d_, d_, t1 - t0, d_, inv + t0);
        for (int t = t0; t < t1; ++t)
            k.scale_mul(X + static_cast<size_t>(t) * d_, inv[t], norm_, Xn + static_cast<size_t>(t) * d_, d_);
    }
    team.sync();
    {
        const auto [o0, o1] = team.share(3 * d_, 8);
        k.gemm(qkv_.matrix(), Xn, d_, T, QKV, 3 * static_cast<size_t>(d_), o0, o1, fold);
    }
    team.sync();
    {
        const auto [t0, t1] = team.share(T);
        for (int t = t0; t < t1; ++t)
            for (int h = 0; h < heads_; ++h) {
                float* row = QKV + static_cast<size_t>(t) * 3 * d_ + h * head_dim_;
                rope(row, t);       // q
                rope(row + d_, t);  // k
            }
    }
    team.sync();
    {
        const float scale = 1.0f / std::sqrt(static_cast<float>(head_dim_));
        const auto [i0, i1] = team.share64(static_cast<int64_t>(heads_) * T);  // may not fit an int
        for (int64_t item = i0; item < i1; ++item) {
            const int h = static_cast<int>(item / T), t = static_cast<int>(item % T);
            const float* q = QKV + static_cast<size_t>(t) * 3 * d_ + h * head_dim_;
            float top = -INFINITY;
            for (int j = 0; j <= t; ++j) {
                const float* kj = QKV + static_cast<size_t>(j) * 3 * d_ + d_ + h * head_dim_;
                float score;
                k.dot_rows(kj, head_dim_, 1, q, head_dim_, &score);
                weights[j] = score * scale;
                top = std::max(top, weights[j]);
            }
            float sum = 0.f;
            for (int j = 0; j <= t; ++j) {
                weights[j] = std::exp(weights[j] - top);
                sum += weights[j];
            }
            float* y = Y + static_cast<size_t>(t) * d_ + h * head_dim_;
            for (int c = 0; c < head_dim_; ++c) y[c] = 0.f;
            for (int j = 0; j <= t; ++j) {
                const float p = weights[j] / sum;
                const float* vj = QKV + static_cast<size_t>(j) * 3 * d_ + 2 * d_ + h * head_dim_;
                for (int c = 0; c < head_dim_; ++c) y[c] = y[c] + p * vj[c];
            }
        }
    }
    team.sync();
    {
        const auto [o0, o1] = team.share(d_, 8);
        k.gemm(out_.matrix(), Y, d_, T, Out, d_, o0, o1, fold);
    }
    team.sync();
    {
        const auto [t0, t1] = team.share(T);
        for (size_t i = static_cast<size_t>(t0) * d_; i < static_cast<size_t>(t1) * d_; ++i) X[i] += Out[i];
    }
    team.sync();
}

std::string AttentionBlock::describe() const {
    json::Writer w;
    w.begin_object()
        .field("type", "attention")
        .field("n_heads", heads_)
        .field("head_dim", head_dim_)
        .field("projections", qkv_.ternary() ? "ternary" : "float")
        .end_object();
    return w.take();
}

}  // namespace

std::unique_ptr<Block> load_attention_block(LoadContext& ctx, int index) {
    return std::make_unique<AttentionBlock>(ctx, index);
}

}  // namespace pb
