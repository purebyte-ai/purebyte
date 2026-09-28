// Bounded lookahead: gives every head the k bytes to the RIGHT of each position, and nothing further.
//
//   y[t] = h[t] + sum_{j=1..k} w[:, j-1] * h[t+j]      (depthwise; h[t+j] = 0 past the last byte of the window)
//
// `look.conv_w` F32 [d_model, k] (column j-1 = offset +j); applied to the output of the final RMSNorm, it feeds every
// head. The sum is accumulated j = 1, 2, ... with a separate multiply and add, then added to h[t].
#pragma once

#include <memory>
#include <string>

#include "model/load_context.h"

namespace pb {

class Lookahead {
public:
    static std::unique_ptr<Lookahead> load(LoadContext& ctx);  // nullptr when the file has none

    int width() const { return k_; }
    // out[t] for t in [t0, t1) from H [T, d_model].
    void apply(const float* H, int T, int t0, int t1, float* out) const;
    std::string describe() const;

private:
    int d_ = 0, k_ = 0;
    const float* w_ = nullptr;  // [d_model, k]
};

}  // namespace pb
