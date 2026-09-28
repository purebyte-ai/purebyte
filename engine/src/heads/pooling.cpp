#include "heads/pooling.h"

#include <cmath>
#include <cstring>
#include <limits>

#include "core/failure.h"

namespace pb {

Pooling read_pooling(const gguf::File& file, int index) {
    const std::string key = LoadContext::key("head", index, "pooling");
    const std::string p = file.text(key, "last_mean");
    if (p == "last") return Pooling::Last;
    if (p == "mean") return Pooling::Mean;
    if (p == "max") return Pooling::Max;
    if (p == "last_mean") return Pooling::LastMean;
    if (p == "max_mean") return Pooling::MaxMean;
    fail(PB_ERR_UNSUPPORTED,
         format("`%s` = `%s` (known: last, mean, max, last_mean, max_mean)", key.c_str(), p.c_str()));
}

const char* pooling_name(Pooling p) {
    switch (p) {
        case Pooling::Last: return "last";
        case Pooling::Mean: return "mean";
        case Pooling::Max: return "max";
        case Pooling::LastMean: return "last_mean";
        case Pooling::MaxMean: return "max_mean";
    }
    return "?";
}

int pooled_width(Pooling p, int d) { return (p == Pooling::LastMean || p == Pooling::MaxMean) ? 2 * d : d; }

void pool(Pooling p, const float* H, int T, int d, float* out) {
    float* first = out;
    const bool doubled = p == Pooling::LastMean || p == Pooling::MaxMean;
    if (p == Pooling::Max || p == Pooling::MaxMean) {
        for (int i = 0; i < d; ++i) first[i] = -1e30f;
        for (int t = 0; t < T; ++t)
            for (int i = 0; i < d; ++i) first[i] = std::fmax(first[i], H[static_cast<size_t>(t) * d + i]);
    } else if (p == Pooling::Mean) {
        for (int i = 0; i < d; ++i) first[i] = 0.f;
        for (int t = 0; t < T; ++t)
            for (int i = 0; i < d; ++i) first[i] += H[static_cast<size_t>(t) * d + i] / T;
    } else {
        std::memcpy(first, H + static_cast<size_t>(T - 1) * d, sizeof(float) * d);
    }
    if (doubled) {
        float* second = out + d;
        for (int i = 0; i < d; ++i) second[i] = 0.f;
        for (int t = 0; t < T; ++t)
            for (int i = 0; i < d; ++i) second[i] += H[static_cast<size_t>(t) * d + i] / T;
    }
}

void PooledMlp::load(LoadContext& ctx, const std::string& prefix, int d_in) {
    const gguf::Tensor& w0 = ctx.tensor(prefix + "mlp.0.weight");
    const gguf::Tensor& w2 = ctx.tensor(prefix + "mlp.2.weight");
    d_in_ = d_in;
    // The pooled width is declared by `pooling`; a matrix of another width means the file contradicts itself (the
    // head would read past its rows and still produce plausible numbers). Dimensions are compared as the file's
    // 64-bit values (a tensor holds at most 2^40 elements, so each fits an int once the other is at least 1).
    if (w0.type != gguf::TensorType::F32 || w0.dims.size() != 2 || w0.dim(0) != static_cast<uint64_t>(d_in) ||
        w0.dim(1) > static_cast<uint64_t>(std::numeric_limits<int>::max()))
        fail(PB_ERR_FORMAT, format("`%smlp.0.weight` has shape %s, expected [hidden, %d] (from the pooling)",
                                   prefix.c_str(), w0.shape().c_str(), d_in));
    hidden_ = static_cast<int>(w0.dim(1));
    if (w2.type != gguf::TensorType::F32 || w2.dims.size() != 2 || w2.dim(0) != static_cast<uint64_t>(hidden_) ||
        w2.dim(1) > static_cast<uint64_t>(std::numeric_limits<int>::max()))
        fail(PB_ERR_FORMAT, format("`%smlp.2.weight` has shape %s, expected [outputs, %d]", prefix.c_str(),
                                   w2.shape().c_str(), hidden_));
    outputs_ = static_cast<int>(w2.dim(1));
    w0_ = w0.f32();
    w2_ = w2.f32();
    b0_ = ctx.f32(prefix + "mlp.0.bias", {hidden_});
    b2_ = ctx.f32(prefix + "mlp.2.bias", {outputs_});
}

void PooledMlp::run(const float* x, float* h, float* out) const {
    for (int o = 0; o < hidden_; ++o) {
        float a = b0_[o];
        const float* w = w0_ + static_cast<size_t>(o) * d_in_;
        for (int i = 0; i < d_in_; ++i) a += w[i] * x[i];
        h[o] = a / (1.0f + std::exp(-a));
    }
    for (int o = 0; o < outputs_; ++o) {
        float a = b2_[o];
        const float* w = w2_ + static_cast<size_t>(o) * hidden_;
        for (int i = 0; i < hidden_; ++i) a += w[i] * h[i];
        out[o] = a;
    }
}

}  // namespace pb
