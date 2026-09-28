// What a block, head or input module needs while it loads itself from a GGUF file: typed metadata, tensors with their
// shapes checked, and the tensor claims that detect leftovers.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "core/numerics.h"  // every part of a model computes numerics: no contraction of a * b + c
#include "format/gguf.h"

namespace pb {

class LoadContext {
public:
    LoadContext(const gguf::File& file, gguf::TensorClaims& claims, int d_model)
        : file_(file), claims_(claims), d_model_(d_model) {}

    const gguf::File& file() const { return file_; }
    int d_model() const { return d_model_; }

    // An F32 tensor whose shape, in row-major (PyTorch) order, is `shape`; a scalar may be stored with shape [] or [1].
    const float* f32(const std::string& name, std::initializer_list<int64_t> shape);
    const float* f32_optional(const std::string& name, std::initializer_list<int64_t> shape);
    // The claimed tensor, whatever its shape (the caller checks it).
    const gguf::Tensor& tensor(const std::string& name) { return claims_.take(name); }
    const gguf::Tensor* tensor_optional(const std::string& name) { return claims_.take_optional(name); }

    // Metadata key of a block or head: "purebyte.<scope>.<index>.<key>".
    static std::string key(const char* scope, int index, const char* key);

private:
    void check_shape(const gguf::Tensor& t, std::initializer_list<int64_t> shape) const;

    const gguf::File& file_;
    gguf::TensorClaims& claims_;
    int d_model_;
};

// Integer metadata within [lo, hi]; PB_ERR_FORMAT naming the key otherwise (missing, for the overload without a
// fallback). The range is checked on the 64-bit value, before any narrowing.
int64_t checked_int(const gguf::File& file, const std::string& key, int64_t lo, int64_t hi);
int64_t checked_int(const gguf::File& file, const std::string& key, int64_t lo, int64_t hi, int64_t fallback);

// Number metadata (integers accepted) as a float: PB_ERR_FORMAT naming the key when it is NaN, infinite or beyond the
// float range (and, for the overload without a fallback, when it is missing).
float checked_finite(const gguf::File& file, const std::string& key);
float checked_finite(const gguf::File& file, const std::string& key, double fallback);
// An array of numbers, each checked the same way; empty when absent.
std::vector<float> checked_finite_list(const gguf::File& file, const std::string& key);

}  // namespace pb
