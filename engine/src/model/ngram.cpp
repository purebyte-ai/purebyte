#include "model/ngram.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/failure.h"
#include "core/json.h"

namespace pb {

namespace {

constexpr uint64_t kModulus = 2147483647ull;  // 2^31 - 1
constexpr uint64_t kBase = 257;

// Far above any trained model (the released ones have at most 262,144 buckets and 192 columns): the shapes come from
// the file, and the scratch buffers are sized from them.
constexpr int64_t kMaxBuckets = int64_t(1) << 24;
constexpr int64_t kMaxColumns = int64_t(1) << 16;  // of the concatenated rows, so of one table too

bool all_digits(const std::string& s) {
    return !s.empty() && s.size() < 9 && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// "ngram.<order>.table" (head -1) or "ngram.<order>.<head>.table".
bool parse_table_name(const std::string& name, int& order, int& head) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t dot; (dot = name.find('.', start)) != std::string::npos; start = dot + 1)
        parts.push_back(name.substr(start, dot - start));
    parts.push_back(name.substr(start));
    if (parts.size() < 3 || parts[0] != "ngram" || parts.back() != "table" || !all_digits(parts[1])) return false;
    order = std::stoi(parts[1]);
    if (parts.size() == 3)
        head = -1;
    else if (parts.size() == 4 && all_digits(parts[2]))
        head = std::stoi(parts[2]);
    else
        return false;
    return true;
}

}  // namespace

std::unique_ptr<NGramInput> NGramInput::load(LoadContext& ctx, int n_layers) {
    const gguf::File& f = ctx.file();
    std::vector<int> orders;
    for (const gguf::Tensor& t : f.tensors()) {
        int order, head;
        if (parse_table_name(t.name, order, head) && std::find(orders.begin(), orders.end(), order) == orders.end())
            orders.push_back(order);
    }
    std::sort(orders.begin(), orders.end());
    // The declared orders (optional) must name exactly the tables present; an empty list declares no n-grams.
    const std::vector<int64_t> declared = f.integers("purebyte.ngram.orders");
    if (f.has("purebyte.ngram.orders") && std::vector<int64_t>(orders.begin(), orders.end()) != declared)
        fail(PB_ERR_FORMAT, "`purebyte.ngram.orders` does not match the n-gram tables of the file");
    if (orders.empty()) {
        if (f.tensor("ngram.proj.weight") || f.flag("purebyte.ngram.gate", false))
            fail(PB_ERR_FORMAT, "the file declares an n-gram projection or gate but has no n-gram table");
        return nullptr;
    }

    auto in = std::unique_ptr<NGramInput>(new NGramInput());
    NGramInput& g = *in;
    g.d_ = ctx.d_model();
    g.orders_ = orders;
    if (orders.front() < 1 || orders.back() > 64) fail(PB_ERR_FORMAT, "n-gram orders must be within 1..64");
    g.hash_heads_ = static_cast<int>(checked_int(f, "purebyte.ngram.heads", 1, 64, 1));
    const std::string hash =
        g.hash_heads_ == 1 ? "poly base=257 mod=2^31-1 seed=1000*i+7" : "poly base=257 mod=2^31-1 seed=1000*i+7+7919*h";
    if (f.text("purebyte.ngram.hash", hash) != hash)
        fail(PB_ERR_UNSUPPORTED, "n-gram hash `" + f.text("purebyte.ngram.hash") + "` (known: " + hash + ")");
    const int64_t bits = f.integer("purebyte.ngram.bits", 32);  // compared before it is narrowed
    if (bits != 32 && bits != 4)
        fail(PB_ERR_UNSUPPORTED, format("purebyte.ngram.bits = %lld (known: 4, 32)", static_cast<long long>(bits)));
    g.bits_ = static_cast<int>(bits);

    for (size_t i = 0; i < orders.size(); ++i)
        for (int h = 0; h < g.hash_heads_; ++h) {
            const std::string base =
                "ngram." + std::to_string(orders[i]) + "." + (g.hash_heads_ == 1 ? "" : std::to_string(h) + ".");
            const gguf::Tensor& t = ctx.tensor(base + "table");
            if (t.dims.size() != 2) fail(PB_ERR_FORMAT, "n-gram table `" + t.name + "` must have two dimensions");
            // 64-bit until checked: a dimension is up to 2^40 (format/gguf.cpp).
            const int64_t buckets = static_cast<int64_t>(t.dim(1));
            const int64_t part = static_cast<int64_t>(t.dim(0)) * (g.bits_ == 4 ? 2 : 1);
            if (buckets > kMaxBuckets || part > kMaxColumns)
                fail(PB_ERR_FORMAT,
                     format("n-gram table `%s` has %lld buckets of %lld columns; this runtime reads at "
                            "most %lld buckets of %lld columns",
                            t.name.c_str(), static_cast<long long>(buckets), static_cast<long long>(part),
                            static_cast<long long>(kMaxBuckets), static_cast<long long>(kMaxColumns)));
            if (g.bits_ == 4) {
                if (t.type != gguf::TensorType::I8)
                    fail(PB_ERR_FORMAT, "4-bit n-gram table `" + t.name + "` must be I8 (packed)");
                g.packed_.push_back(t.data);
                g.scales_.push_back(ctx.f32(base + "scale", {buckets}));
            } else {
                if (t.type != gguf::TensorType::F32) fail(PB_ERR_FORMAT, "n-gram table `" + t.name + "` must be F32");
                g.tables_.push_back(t.f32());
            }
            if ((g.buckets_ && (buckets != g.buckets_ || part != g.part_)) || buckets < 1 || part < 1)
                fail(PB_ERR_FORMAT, "the n-gram tables have different shapes");
            g.buckets_ = static_cast<int>(buckets);
            g.part_ = static_cast<int>(part);
        }
    const int64_t declared_buckets = f.integer("purebyte.ngram.buckets", 0),
                  declared_dim = f.integer("purebyte.ngram.dim", 0);
    if ((declared_buckets && declared_buckets != g.buckets_) ||
        (declared_dim && declared_dim != static_cast<int64_t>(g.part_) * g.hash_heads_))
        fail(PB_ERR_FORMAT, "`purebyte.ngram.buckets` / `dim` do not match the n-gram tables");
    // Up to 64 orders x 64 heads x 2^16 columns: 64-bit, then within the limit.
    const int64_t width = static_cast<int64_t>(orders.size()) * g.hash_heads_ * g.part_;
    if (width > kMaxColumns)
        fail(PB_ERR_FORMAT, format("the n-gram rows add up to %lld columns (%zu orders x %d heads x %d); this runtime "
                                   "reads at most %lld",
                                   static_cast<long long>(width), orders.size(), g.hash_heads_, g.part_,
                                   static_cast<long long>(kMaxColumns)));
    g.width_ = static_cast<int>(width);
    g.proj_ = ctx.f32("ngram.proj.weight", {g.d_, g.width_});
    g.layer_ = static_cast<int>(checked_int(f, "purebyte.ngram.layer", 0, n_layers - 1, 0));
    g.gated_ = f.flag("purebyte.ngram.gate", false);
    if (g.gated_) g.gate_bias_ = *ctx.f32("ngram.gate_bias", {});
    return in;
}

void NGramInput::apply(const kernels::Kernels& k, const uint8_t* bytes, int history, int t0, int t1, float* X,
                       float* scratch) const {
    float* cat = scratch;
    float* e = cat + width_;
    float* rx = e + d_;
    float* re = rx + d_;
    for (int t = t0; t < t1; ++t) {
        int column = 0;
        for (size_t i = 0; i < orders_.size(); ++i)
            for (int h = 0; h < hash_heads_; ++h) {
                uint64_t v = static_cast<uint64_t>(1000 * i + 7 + 7919 * h) % kModulus;
                for (int back = orders_[i] - 1; back >= 0; --back) {
                    const int pos = t - back;
                    const uint64_t byte = pos >= -history ? bytes[pos] : 0;
                    v = (v * kBase + byte + 1) % kModulus;
                }
                const size_t row = static_cast<size_t>(v % static_cast<uint64_t>(buckets_));
                const size_t table = i * hash_heads_ + h;
                float* c = cat + column;
                if (bits_ == 4) {
                    const uint8_t* p = packed_[table] + row * static_cast<size_t>(part_ / 2);
                    const float s = scales_[table][row];
                    for (int j = 0; j < part_ / 2; ++j) {
                        const int lo = p[j] & 0xF, hi = p[j] >> 4;
                        c[2 * j] = s * static_cast<float>(lo >= 8 ? lo - 16 : lo);
                        c[2 * j + 1] = s * static_cast<float>(hi >= 8 ? hi - 16 : hi);
                    }
                } else {
                    std::memcpy(c, tables_[table] + row * static_cast<size_t>(part_), sizeof(float) * part_);
                }
                column += part_;
            }
        k.dot_rows(proj_, width_, d_, cat, width_, e);
        float* x = X + static_cast<size_t>(t) * d_;
        if (!gated_) {
            for (int o = 0; o < d_; ++o) x[o] += e[o];
            continue;
        }
        float inv_x, inv_e;
        k.rms_inv(x, d_, 1, d_, &inv_x);
        k.rms_inv(e, d_, 1, d_, &inv_e);
        for (int o = 0; o < d_; ++o) {
            rx[o] = x[o] * inv_x;
            re[o] = e[o] * inv_e;
        }
        float s;
        k.dot_rows(rx, d_, 1, re, d_, &s);
        const float a = 1.0f / (1.0f + std::exp(-(s / std::sqrt(static_cast<float>(d_)) + gate_bias_)));
        for (int o = 0; o < d_; ++o) x[o] += a * e[o];
    }
}

std::string NGramInput::describe() const {
    json::Writer w;
    w.begin_object().key("orders").begin_array();
    for (int n : orders_) w.integer(n);
    w.end_array()
        .field("buckets", buckets_)
        .field("dim", part_ * hash_heads_)
        .field("hash_heads", hash_heads_)
        .field("bits", bits_)
        .field("layer", layer_)
        .field("gated", gated_)
        .end_object();
    return w.take();
}

}  // namespace pb
