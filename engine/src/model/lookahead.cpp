#include "model/lookahead.h"

#include "core/failure.h"
#include "core/json.h"

namespace pb {

std::unique_ptr<Lookahead> Lookahead::load(LoadContext& ctx) {
    const gguf::Tensor* t = ctx.tensor_optional("look.conv_w");
    if (!t) {
        if (ctx.file().has("purebyte.lookahead"))
            fail(PB_ERR_FORMAT, "`purebyte.lookahead` is set but `look.conv_w` is missing");
        return nullptr;
    }
    auto look = std::unique_ptr<Lookahead>(new Lookahead());
    look->d_ = ctx.d_model();
    // The file's 64-bit dimensions, checked before they are narrowed.
    if (t->type != gguf::TensorType::F32 || t->dims.size() != 2 || t->dim(1) != static_cast<uint64_t>(look->d_) ||
        t->dim(0) < 1 || t->dim(0) > 1024)
        fail(PB_ERR_FORMAT,
             format("`look.conv_w` has shape %s, expected [%d, k] with k = 1..1024", t->shape().c_str(), look->d_));
    look->k_ = static_cast<int>(t->dim(0));
    if (ctx.file().integer("purebyte.lookahead", look->k_) != look->k_)
        fail(PB_ERR_FORMAT, "`purebyte.lookahead` does not match the width of `look.conv_w`");
    look->w_ = t->f32();
    return look;
}

void Lookahead::apply(const float* H, int T, int t0, int t1, float* out) const {
    for (int t = t0; t < t1; ++t) {
        const float* h = H + static_cast<size_t>(t) * d_;
        float* y = out + static_cast<size_t>(t) * d_;
        for (int c = 0; c < d_; ++c) {
            const float* w = w_ + static_cast<size_t>(c) * k_;
            float acc = 0.f;
            for (int j = 1; j <= k_; ++j) {
                const float right = t + j < T ? H[static_cast<size_t>(t + j) * d_ + c] : 0.f;
                const float term = w[j - 1] * right;
                acc = j == 1 ? term : acc + term;
            }
            y[c] = h[c] + acc;
        }
    }
}

std::string Lookahead::describe() const {
    json::Writer w;
    w.begin_object().field("width", k_).end_object();
    return w.take();
}

}  // namespace pb
