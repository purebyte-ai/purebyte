// Hashed byte n-gram tables: domain knowledge as RAM look-ups added to the residual stream.
//
// For each order n (ascending) and hash head h, the n-gram that ENDS at position t (bytes before the start of the
// sequence count as 0) is hashed polynomially, base 257 modulo 2^31-1, from the seed 1000*i + 7 + 7919*h (i = index of
// the order), and selects a row of that table. The rows are concatenated (order-major, then head) and projected to
// d_model by `ngram.proj.weight`. Tables are F32, or 4-bit (two's-complement nibbles, the even column in the low
// nibble, one F32 scale per row: value = scale * code).
//
// The vector is added before block `purebyte.ngram.layer` (0 = right after the byte embedding). With the context gate
// (`purebyte.ngram.gate`) it is scaled by a = sigmoid(<rms(x), rms(e)> / sqrt(d_model) + gate_bias) first.
//
// A file with tables of more than 2^24 buckets, or rows that add up to more than 65,536 columns, is refused.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "kernels/kernels.h"
#include "model/load_context.h"

namespace pb {

class NGramInput {
public:
    // nullptr when the file has no n-gram tables.
    static std::unique_ptr<NGramInput> load(LoadContext& ctx, int n_layers);

    int layer() const { return layer_; }
    int max_order() const { return orders_.back(); }
    size_t member_scratch_floats() const { return static_cast<size_t>(width_) + 3 * static_cast<size_t>(d_); }

    // Adds the n-gram vectors of positions [t0, t1) to X [*, d_model]. bytes[t] is the byte at position t; when
    // streaming, bytes[-history .. -1] are the bytes that precede the chunk (earlier ones count as 0).
    void apply(const kernels::Kernels& k, const uint8_t* bytes, int history, int t0, int t1, float* X,
               float* member_scratch) const;

    std::string describe() const;

private:
    int d_ = 0;
    std::vector<int> orders_;  // ascending
    int hash_heads_ = 1;
    int buckets_ = 0;
    int part_ = 0;   // columns of one table
    int width_ = 0;  // columns of the concatenation
    int bits_ = 32;
    std::vector<const float*> tables_;    // F32 tables, [order * heads + head]
    std::vector<const uint8_t*> packed_;  // 4-bit tables
    std::vector<const float*> scales_;    // their row scales
    const float* proj_ = nullptr;         // [d_model, width]
    int layer_ = 0;
    bool gated_ = false;
    float gate_bias_ = 0.f;
};

}  // namespace pb
