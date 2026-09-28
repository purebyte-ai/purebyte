#include "model/load_context.h"

#include <cmath>
#include <limits>

#include "core/failure.h"

namespace pb {

void LoadContext::check_shape(const gguf::Tensor& t, std::initializer_list<int64_t> shape) const {
    if (t.type != gguf::TensorType::F32) fail(PB_ERR_FORMAT, format("tensor `%s` must be F32", t.name.c_str()));
    // GGUF stores the fastest dimension first; `shape` is row-major. A scalar may be stored as [] or [1].
    std::vector<uint64_t> want(shape.begin(), shape.end());
    bool ok;
    if (want.empty()) {
        ok = t.dims.empty() || (t.dims.size() == 1 && t.dims[0] == 1);
    } else {
        ok = t.dims.size() == want.size();
        for (size_t i = 0; ok && i < want.size(); ++i) ok = t.dims[want.size() - 1 - i] == want[i];
    }
    if (!ok) {
        std::string expected = "[";
        for (size_t i = 0; i < want.size(); ++i)
            expected += std::to_string(want[i]) + (i + 1 < want.size() ? ", " : "");
        fail(PB_ERR_FORMAT,
             format("tensor `%s` has shape %s, expected %s]", t.name.c_str(), t.shape().c_str(), expected.c_str()));
    }
}

const float* LoadContext::f32(const std::string& name, std::initializer_list<int64_t> shape) {
    const gguf::Tensor& t = claims_.take(name);
    check_shape(t, shape);
    return t.f32();
}

const float* LoadContext::f32_optional(const std::string& name, std::initializer_list<int64_t> shape) {
    const gguf::Tensor* t = claims_.take_optional(name);
    if (!t) return nullptr;
    check_shape(*t, shape);
    return t->f32();
}

std::string LoadContext::key(const char* scope, int index, const char* key) {
    return std::string("purebyte.") + scope + "." + std::to_string(index) + "." + key;
}

int64_t checked_int(const gguf::File& file, const std::string& key, int64_t lo, int64_t hi) {
    const int64_t v = file.integer(key);
    if (v < lo || v > hi)
        fail(PB_ERR_FORMAT, format("metadata `%s` = %lld is outside [%lld, %lld]", key.c_str(),
                                   static_cast<long long>(v), static_cast<long long>(lo), static_cast<long long>(hi)));
    return v;
}

int64_t checked_int(const gguf::File& file, const std::string& key, int64_t lo, int64_t hi, int64_t fallback) {
    return file.has(key) ? checked_int(file, key, lo, hi) : fallback;
}

namespace {

// A double that a float holds without overflow; the conversion of a larger one is undefined in C++.
float finite_float(double v, const std::string& key) {
    if (!std::isfinite(v) || std::fabs(v) > static_cast<double>(std::numeric_limits<float>::max()))
        fail(PB_ERR_FORMAT, format("metadata `%s` must be a finite number within the float range", key.c_str()));
    return static_cast<float>(v);
}

}  // namespace

float checked_finite(const gguf::File& file, const std::string& key) { return finite_float(file.real(key), key); }

float checked_finite(const gguf::File& file, const std::string& key, double fallback) {
    return finite_float(file.real(key, fallback), key);
}

std::vector<float> checked_finite_list(const gguf::File& file, const std::string& key) {
    std::vector<float> out;
    for (double v : file.reals(key)) out.push_back(finite_float(v, key));
    return out;
}

}  // namespace pb
