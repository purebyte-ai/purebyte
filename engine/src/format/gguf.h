// GGUF v3 container reader (https://github.com/ggml-org/ggml/blob/master/docs/gguf.md).
//
// A model file is downloaded from the network: nothing in its header is trusted. Every length, count, offset and
// dimension is bounds-checked before use, and anything malformed is a PB_ERR_FORMAT, never undefined behaviour.
// The reader knows the container only; what the keys and tensors MEAN is the business of the model loader
// (model/model.cpp and the parts it loads through model/load_context.h).
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace pb::gguf {

// Metadata value types of the GGUF specification.
enum class ValueType : uint32_t {
    U8 = 0,
    I8 = 1,
    U16 = 2,
    I16 = 3,
    U32 = 4,
    I32 = 5,
    F32 = 6,
    Bool = 7,
    String = 8,
    Array = 9,
    U64 = 10,
    I64 = 11,
    F64 = 12,
};

const char* value_type_name(ValueType type);

struct Value {
    ValueType type = ValueType::U8;
    ValueType element = ValueType::U8;  // element type, for arrays
    int64_t integer = 0;                // integers and booleans
    double real = 0.0;                  // F32 and F64
    std::string text;                   // strings
    std::vector<Value> items;           // arrays

    bool is_integer() const;
    bool is_real() const;
};

// Tensor element types this format uses. The GGML type ids are kept so that standard GGUF tools can read the file.
enum class TensorType : uint32_t { F32 = 0, I8 = 24 };

struct Tensor {
    std::string name;
    std::vector<uint64_t> dims;  // GGUF order: the fastest-varying dimension first
    TensorType type = TensorType::F32;
    uint64_t count = 0;  // elements
    uint64_t bytes = 0;
    const uint8_t* data = nullptr;

    // Dimension i, or 1 beyond the tensor's rank (a [n] vector is a [1, n] matrix).
    uint64_t dim(size_t i) const { return i < dims.size() ? dims[i] : 1; }
    const float* f32() const { return reinterpret_cast<const float*>(data); }
    std::string shape() const;  // "[rows, cols]" in row-major (PyTorch) order, for messages
};

class File {
public:
    // Reads the whole file (path in UTF-8) and parses it.
    static std::shared_ptr<const File> open(const std::string& path);
    // Parses a copy of `size` bytes.
    static std::shared_ptr<const File> from_memory(const void* data, size_t size);

    const Value* find(const std::string& key) const;
    bool has(const std::string& key) const { return find(key) != nullptr; }
    const std::map<std::string, Value>& metadata() const { return metadata_; }

    // Typed access. A missing key yields the fallback (or a PB_ERR_FORMAT for the overloads without one); a key of
    // the wrong type is always a PB_ERR_FORMAT: a value this library cannot read must not become a silent default.
    int64_t integer(const std::string& key) const;
    int64_t integer(const std::string& key, int64_t fallback) const;
    double real(const std::string& key, double fallback) const;
    double real(const std::string& key) const;
    std::string text(const std::string& key, const std::string& fallback) const;
    std::string text(const std::string& key) const;
    bool flag(const std::string& key, bool fallback) const;
    std::vector<std::string> texts(const std::string& key) const;  // array of strings; empty when absent
    std::vector<double> reals(const std::string& key) const;       // array of numbers; empty when absent
    std::vector<int64_t> integers(const std::string& key) const;   // array of integers; empty when absent

    const Tensor* tensor(const std::string& name) const;
    const std::vector<Tensor>& tensors() const { return tensors_; }
    size_t size() const { return blob_.size(); }

private:
    File() = default;
    void parse();

    std::vector<uint8_t> blob_;
    std::map<std::string, Value> metadata_;
    std::vector<Tensor> tensors_;
    std::map<std::string, size_t> index_;
};

// Tracks which tensors a loader consumed. A tensor nobody claims means the file holds something this library would
// ignore: the model would compute a different function than the one it was exported from, so loading refuses it.
class TensorClaims {
public:
    explicit TensorClaims(const File& file) : file_(file), claimed_(file.tensors().size(), false) {}
    const Tensor& take(const std::string& name);           // throws PB_ERR_FORMAT when missing
    const Tensor* take_optional(const std::string& name);  // nullptr when missing
    std::vector<std::string> unclaimed() const;

private:
    const File& file_;
    std::vector<bool> claimed_;
};

}  // namespace pb::gguf
