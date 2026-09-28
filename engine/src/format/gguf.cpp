#include "format/gguf.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "core/failure.h"
#include "core/files.h"

// Model files, and the digests of their numbers, are little-endian; this reader copies them as they are.
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#error "PureByte reads its model files in little-endian byte order: big-endian targets are not supported"
#endif

namespace pb::gguf {

namespace {

constexpr uint32_t kMagic = 0x46554747u;  // "GGUF", little-endian
constexpr uint32_t kVersion = 3;
constexpr uint64_t kDefaultAlignment = 32;
constexpr uint64_t kMaxEntries = 1u << 20;  // metadata keys or tensors: far above any real model
constexpr uint64_t kMaxValues = 1u << 20;  // metadata values, array items included: bounds the memory of a hostile file
constexpr int kMaxArrayDepth = 4;
constexpr size_t kMaxRank = 4;  // GGML_MAX_DIMS

[[noreturn]] void corrupt(const std::string& what) { fail(PB_ERR_FORMAT, "invalid GGUF: " + what); }

// Bounds-checked little-endian cursor over the file.
class Cursor {
public:
    Cursor(const uint8_t* begin, const uint8_t* end) : p_(begin), begin_(begin), end_(end) {}

    template <class T>
    T read() {
        need(sizeof(T));
        T v;
        std::memcpy(&v, p_, sizeof(T));
        p_ += sizeof(T);
        return v;
    }
    std::string string() {
        const uint64_t n = read<uint64_t>();
        need(n);
        std::string s(reinterpret_cast<const char*>(p_), static_cast<size_t>(n));
        p_ += n;
        return s;
    }
    size_t offset() const { return static_cast<size_t>(p_ - begin_); }

private:
    void need(uint64_t n) const {
        if (n > static_cast<uint64_t>(end_ - p_)) corrupt(format("truncated at byte %zu", offset()));
    }
    const uint8_t* p_;
    const uint8_t* begin_;
    const uint8_t* end_;
};

Value read_value(Cursor& in, ValueType type, int depth, uint64_t& budget) {
    if (budget == 0) corrupt("too many metadata values");
    --budget;
    Value v;
    v.type = type;
    switch (type) {
        case ValueType::U8: v.integer = in.read<uint8_t>(); break;
        case ValueType::I8: v.integer = in.read<int8_t>(); break;
        case ValueType::U16: v.integer = in.read<uint16_t>(); break;
        case ValueType::I16: v.integer = in.read<int16_t>(); break;
        case ValueType::U32: v.integer = in.read<uint32_t>(); break;
        case ValueType::I32: v.integer = in.read<int32_t>(); break;
        case ValueType::I64: v.integer = in.read<int64_t>(); break;
        case ValueType::U64: {
            const uint64_t u = in.read<uint64_t>();
            if (u > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) corrupt("integer out of range");
            v.integer = static_cast<int64_t>(u);
            break;
        }
        case ValueType::Bool: {
            const uint8_t b = in.read<uint8_t>();
            if (b > 1) corrupt("boolean that is neither 0 nor 1");
            v.integer = b;
            break;
        }
        case ValueType::F32: v.real = in.read<float>(); break;
        case ValueType::F64: v.real = in.read<double>(); break;
        case ValueType::String: v.text = in.string(); break;
        case ValueType::Array: {
            if (depth >= kMaxArrayDepth) corrupt("arrays nested too deeply");
            const uint32_t element = in.read<uint32_t>();
            if (element > static_cast<uint32_t>(ValueType::F64))
                corrupt(format("unknown array element type %u", element));
            v.element = static_cast<ValueType>(element);
            const uint64_t n = in.read<uint64_t>();
            if (n > (1u << 28)) corrupt("array too long");
            v.items.reserve(static_cast<size_t>(std::min<uint64_t>(n, 1u << 16)));
            for (uint64_t k = 0; k < n; ++k) v.items.push_back(read_value(in, v.element, depth + 1, budget));
            break;
        }
        default: corrupt(format("unknown metadata type %u", static_cast<uint32_t>(type)));
    }
    return v;
}

[[noreturn]] void wrong_type(const std::string& key, const Value& v, const char* expected) {
    fail(PB_ERR_FORMAT, format("metadata `%s` is %s, expected %s", key.c_str(), value_type_name(v.type), expected));
}

}  // namespace

const char* value_type_name(ValueType type) {
    static const char* names[] = {"u8",   "i8",     "u16",   "i16", "u32", "i32", "f32",
                                  "bool", "string", "array", "u64", "i64", "f64"};
    const auto i = static_cast<uint32_t>(type);
    return i < sizeof names / sizeof names[0] ? names[i] : "unknown";
}

bool Value::is_integer() const {
    switch (type) {
        case ValueType::U8:
        case ValueType::I8:
        case ValueType::U16:
        case ValueType::I16:
        case ValueType::U32:
        case ValueType::I32:
        case ValueType::U64:
        case ValueType::I64: return true;
        default: return false;
    }
}

bool Value::is_real() const { return type == ValueType::F32 || type == ValueType::F64; }

std::string Tensor::shape() const {
    std::string s = "[";
    for (size_t i = dims.size(); i-- > 0;) s += std::to_string(dims[i]) + (i ? ", " : "");
    return s + "]";
}

std::shared_ptr<const File> File::open(const std::string& path) {
    std::shared_ptr<File> f(new File());
    f->blob_ = read_file(path);
    f->parse();
    return f;
}

std::shared_ptr<const File> File::from_memory(const void* data, size_t size) {
    std::shared_ptr<File> f(new File());
    const auto* p = static_cast<const uint8_t*>(data);
    f->blob_.assign(p, p + size);
    f->parse();
    return f;
}

void File::parse() {
    Cursor in(blob_.data(), blob_.data() + blob_.size());
    if (blob_.size() < 24 || in.read<uint32_t>() != kMagic) fail(PB_ERR_FORMAT, "not a GGUF file (bad magic)");
    const uint32_t version = in.read<uint32_t>();
    if (version != kVersion)
        fail(PB_ERR_UNSUPPORTED, format("GGUF version %u; this library reads version %u", version, kVersion));
    const uint64_t n_tensors = in.read<uint64_t>(), n_metadata = in.read<uint64_t>();
    if (n_tensors > kMaxEntries || n_metadata > kMaxEntries) corrupt("absurd tensor or metadata count");

    uint64_t budget = kMaxValues;
    for (uint64_t k = 0; k < n_metadata; ++k) {
        std::string key = in.string();
        const uint32_t type = in.read<uint32_t>();
        if (type > static_cast<uint32_t>(ValueType::F64))
            corrupt(format("key `%s` has unknown type %u", key.c_str(), type));
        Value value = read_value(in, static_cast<ValueType>(type), 0, budget);
        if (!metadata_.emplace(key, std::move(value)).second)
            corrupt(format("metadata key `%s` appears twice", key.c_str()));
    }

    uint64_t alignment = kDefaultAlignment;
    if (const Value* a = find("general.alignment")) {
        // GGUF requires a multiple of 8; F32 tensors are read in place, so their data must be at least 4-aligned.
        if (!a->is_integer() || a->integer < 8 || (a->integer & (a->integer - 1)) != 0 || a->integer > 65536)
            corrupt("general.alignment must be a power of two within 8..65536");
        alignment = static_cast<uint64_t>(a->integer);
    }

    struct Pending {
        uint64_t offset;
    };
    std::vector<Pending> offsets;
    // No more tensors than the rest of the file can describe (a tensor info takes at least 24 bytes): a header that
    // declares 2^20 of them must not reserve their memory before the first one is read.
    constexpr uint64_t kSmallestTensorInfo = 8 + 4 + 4 + 8;  // empty name, rank 0, type, offset
    const uint64_t describable = (blob_.size() - in.offset()) / kSmallestTensorInfo;
    tensors_.reserve(static_cast<size_t>(std::min<uint64_t>(n_tensors, describable)));
    for (uint64_t k = 0; k < n_tensors; ++k) {
        Tensor t;
        t.name = in.string();
        const uint32_t rank = in.read<uint32_t>();
        if (rank > kMaxRank) corrupt(format("tensor `%s` has %u dimensions", t.name.c_str(), rank));
        t.count = 1;
        for (uint32_t d = 0; d < rank; ++d) {
            const uint64_t n = in.read<uint64_t>();
            if (n == 0 || n > (1ull << 40) || t.count > (1ull << 40) / n)
                corrupt(format("tensor `%s` declares impossible dimensions", t.name.c_str()));
            t.dims.push_back(n);
            t.count *= n;
        }
        const uint32_t type = in.read<uint32_t>();
        if (type != static_cast<uint32_t>(TensorType::F32) && type != static_cast<uint32_t>(TensorType::I8))
            fail(PB_ERR_UNSUPPORTED,
                 format("tensor `%s` has GGML type %u; PureByte models use F32 (0) and I8 (24)", t.name.c_str(), type));
        t.type = static_cast<TensorType>(type);
        t.bytes = t.count * (t.type == TensorType::F32 ? 4u : 1u);
        offsets.push_back({in.read<uint64_t>()});
        if (!index_.emplace(t.name, tensors_.size()).second)
            corrupt(format("tensor `%s` appears twice", t.name.c_str()));
        tensors_.push_back(std::move(t));
    }

    const uint64_t header = in.offset();
    const uint64_t base = (header + alignment - 1) / alignment * alignment;
    const uint64_t size = blob_.size();
    for (size_t k = 0; k < tensors_.size(); ++k) {
        Tensor& t = tensors_[k];
        const uint64_t off = offsets[k].offset;
        if (off % alignment != 0)
            corrupt(format("tensor `%s` is not aligned to %llu bytes", t.name.c_str(),
                           static_cast<unsigned long long>(alignment)));
        // The offset comes FROM THE FILE: a truncated or tampered download must fail here, not read past the end.
        if (base > size || off > size - base || t.bytes > size - base - off)
            corrupt(
                format("tensor `%s` claims %llu bytes at offset %llu and the file has %llu: it is truncated or "
                       "tampered with",
                       t.name.c_str(), static_cast<unsigned long long>(t.bytes),
                       static_cast<unsigned long long>(base + off), static_cast<unsigned long long>(size)));
        t.data = blob_.data() + base + off;
    }
    // No two tensors share a byte. Every size is checked tensor by tensor; tensors laid over the same bytes would let a
    // small file declare far more data than it holds, and the loaders size buffers from what it declares.
    std::vector<size_t> order(tensors_.size());
    for (size_t k = 0; k < order.size(); ++k) order[k] = k;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return offsets[a].offset != offsets[b].offset ? offsets[a].offset < offsets[b].offset : a < b;
    });
    for (size_t k = 1; k < order.size(); ++k) {
        const size_t a = order[k - 1], b = order[k];
        if (offsets[a].offset + tensors_[a].bytes > offsets[b].offset)
            corrupt(format("tensors `%s` and `%s` overlap in the file: it is corrupt or tampered with",
                           tensors_[a].name.c_str(), tensors_[b].name.c_str()));
    }
}

const Value* File::find(const std::string& key) const {
    const auto it = metadata_.find(key);
    return it == metadata_.end() ? nullptr : &it->second;
}

int64_t File::integer(const std::string& key) const {
    const Value* v = find(key);
    if (!v) fail(PB_ERR_FORMAT, format("metadata `%s` is missing", key.c_str()));
    return integer(key, 0);
}

int64_t File::integer(const std::string& key, int64_t fallback) const {
    const Value* v = find(key);
    if (!v) return fallback;
    if (!v->is_integer()) wrong_type(key, *v, "an integer");
    return v->integer;
}

double File::real(const std::string& key, double fallback) const {
    const Value* v = find(key);
    if (!v) return fallback;
    if (v->is_real()) return v->real;
    if (v->is_integer()) return static_cast<double>(v->integer);
    wrong_type(key, *v, "a number");
}

double File::real(const std::string& key) const {
    if (!find(key)) fail(PB_ERR_FORMAT, format("metadata `%s` is missing", key.c_str()));
    return real(key, 0.0);
}

std::string File::text(const std::string& key, const std::string& fallback) const {
    const Value* v = find(key);
    if (!v) return fallback;
    if (v->type != ValueType::String) wrong_type(key, *v, "a string");
    return v->text;
}

std::string File::text(const std::string& key) const {
    if (!find(key)) fail(PB_ERR_FORMAT, format("metadata `%s` is missing", key.c_str()));
    return text(key, std::string());
}

bool File::flag(const std::string& key, bool fallback) const {
    const Value* v = find(key);
    if (!v) return fallback;
    if (v->type != ValueType::Bool && !v->is_integer()) wrong_type(key, *v, "a boolean");
    if (v->integer != 0 && v->integer != 1) fail(PB_ERR_FORMAT, format("metadata `%s` must be 0 or 1", key.c_str()));
    return v->integer != 0;
}

std::vector<std::string> File::texts(const std::string& key) const {
    std::vector<std::string> out;
    const Value* v = find(key);
    if (!v) return out;
    if (v->type != ValueType::Array || (v->element != ValueType::String && !v->items.empty()))
        wrong_type(key, *v, "an array of strings");
    for (const Value& x : v->items) out.push_back(x.text);
    return out;
}

std::vector<double> File::reals(const std::string& key) const {
    std::vector<double> out;
    const Value* v = find(key);
    if (!v) return out;
    if (v->type != ValueType::Array) wrong_type(key, *v, "an array of numbers");
    for (const Value& x : v->items) {
        if (x.is_real())
            out.push_back(x.real);
        else if (x.is_integer())
            out.push_back(static_cast<double>(x.integer));
        else
            wrong_type(key, *v, "an array of numbers");
    }
    return out;
}

std::vector<int64_t> File::integers(const std::string& key) const {
    std::vector<int64_t> out;
    const Value* v = find(key);
    if (!v) return out;
    if (v->type != ValueType::Array) wrong_type(key, *v, "an array of integers");
    for (const Value& x : v->items) {
        if (!x.is_integer()) wrong_type(key, *v, "an array of integers");
        out.push_back(x.integer);
    }
    return out;
}

const Tensor* File::tensor(const std::string& name) const {
    const auto it = index_.find(name);
    return it == index_.end() ? nullptr : &tensors_[it->second];
}

const Tensor& TensorClaims::take(const std::string& name) {
    const Tensor* t = take_optional(name);
    if (!t) fail(PB_ERR_FORMAT, format("tensor `%s` is missing", name.c_str()));
    return *t;
}

const Tensor* TensorClaims::take_optional(const std::string& name) {
    const Tensor* t = file_.tensor(name);
    if (t) claimed_[static_cast<size_t>(t - file_.tensors().data())] = true;
    return t;
}

std::vector<std::string> TensorClaims::unclaimed() const {
    std::vector<std::string> out;
    for (size_t k = 0; k < claimed_.size(); ++k)
        if (!claimed_[k]) out.push_back(file_.tensors()[k].name);
    return out;
}

}  // namespace pb::gguf
