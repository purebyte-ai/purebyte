// The trunk shared by the window-level heads (choice, multilabel, score, ordinal): pool the hidden states of the
// window into one vector, then (except ordinal) an MLP  Linear -> SiLU -> Linear.
#pragma once

#include <string>
#include <vector>

#include "model/load_context.h"

namespace pb {

enum class Pooling { Last, Mean, Max, LastMean, MaxMean };

// Reads `purebyte.head.<index>.pooling` (default `last_mean`, the reference's default).
Pooling read_pooling(const gguf::File& file, int index);
const char* pooling_name(Pooling p);
int pooled_width(Pooling p, int d_model);

// out[pooled_width] from H [T, d]: `max` starts from -1e30 and uses fmax; `mean` accumulates h / T in position order.
void pool(Pooling p, const float* H, int T, int d, float* out);

// Linear(d_in -> hidden) -> SiLU -> Linear(hidden -> n), float, accumulated in element order with separate multiply
// and add; SiLU with the C library's expf.
class PooledMlp {
public:
    // Tensors `<prefix>mlp.0.{weight,bias}` and `<prefix>mlp.2.{weight,bias}`; the widths come from their shapes.
    void load(LoadContext& ctx, const std::string& prefix, int d_in);
    int outputs() const { return outputs_; }
    int hidden() const { return hidden_; }
    void run(const float* x, float* hidden_scratch, float* out) const;

private:
    int d_in_ = 0, hidden_ = 0, outputs_ = 0;
    const float *w0_ = nullptr, *b0_ = nullptr, *w2_ = nullptr, *b2_ = nullptr;
};

}  // namespace pb
