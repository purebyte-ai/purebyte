// Model files built in memory for the tests (spec/FORMAT.md): a GGUF writer whose keys and tensors can be replaced or
// dropped (to derive malformed variants), and "flag" models whose tag head marks every byte of a chosen class as a
// one-byte span of entity 0, over a body that adds exactly 0 to the residual stream. End-to-end tests are then exact:
// the spans are known in advance, whatever the kernel.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "purebyte/pb.h"

namespace pbtest {

class GgufBuilder {
public:
    // Setting a key or a tensor that exists replaces it, in place.
    void i32(const std::string& key, int32_t v) { scalar(key, 5, &v, 4); }
    void i64(const std::string& key, int64_t v) { scalar(key, 11, &v, 8); }
    void f32(const std::string& key, float v) { scalar(key, 6, &v, 4); }
    void text(const std::string& key, const std::string& v) {
        std::vector<uint8_t> body;
        u32(body, 8);
        name(body, v);
        set(key, body);
    }
    void texts(const std::string& key, const std::vector<std::string>& v) {
        std::vector<uint8_t> body;
        u32(body, 9);
        u32(body, 8);
        u64(body, v.size());
        for (const std::string& s : v) name(body, s);
        set(key, body);
    }
    void floats(const std::string& key, const std::vector<float>& v) {
        std::vector<uint8_t> body;
        u32(body, 9);
        u32(body, 6);
        u64(body, v.size());
        for (float x : v) put(body, &x, 4);
        set(key, body);
    }
    void drop_key(const std::string& key) {
        meta_.erase(std::remove_if(meta_.begin(), meta_.end(), [&](const Entry& e) { return e.first == key; }),
                    meta_.end());
    }
    // An F32 tensor; `shape` in row-major order (as the specification writes it), `values` row-major too.
    void tensor(const std::string& tensor_name, const std::vector<uint64_t>& shape, const std::vector<float>& values) {
        std::vector<uint8_t> data(values.size() * 4);
        if (!values.empty()) std::memcpy(data.data(), values.data(), data.size());
        raw_tensor(tensor_name, std::vector<uint64_t>(shape.rbegin(), shape.rend()), 0, data);
    }
    // Any tensor: `dims` in GGUF order (fastest-varying first), `type` a GGML type, `data` its bytes.
    void raw_tensor(const std::string& tensor_name, const std::vector<uint64_t>& dims, uint32_t type,
                    const std::vector<uint8_t>& data) {
        Tensor t{tensor_name, dims, type, data};
        for (Tensor& old : tensors_)
            if (old.name == tensor_name) {
                old = t;
                return;
            }
        tensors_.push_back(t);
    }
    void drop_tensor(const std::string& tensor_name) {
        tensors_.erase(
            std::remove_if(tensors_.begin(), tensors_.end(), [&](const Tensor& t) { return t.name == tensor_name; }),
            tensors_.end());
    }
    std::vector<uint8_t> bytes() const {
        std::vector<uint8_t> out, infos, data;
        for (const Tensor& t : tensors_) {
            name(infos, t.name);
            u32(infos, static_cast<uint32_t>(t.dims.size()));
            for (uint64_t d : t.dims) u64(infos, d);
            u32(infos, t.type);
            u64(infos, data.size());
            data.insert(data.end(), t.data.begin(), t.data.end());
            data.resize((data.size() + 31) / 32 * 32, 0);
        }
        u32(out, 0x46554747);
        u32(out, 3);
        u64(out, tensors_.size());
        u64(out, meta_.size());
        for (const Entry& e : meta_) {
            name(out, e.first);
            out.insert(out.end(), e.second.begin(), e.second.end());
        }
        out.insert(out.end(), infos.begin(), infos.end());
        out.resize((out.size() + 31) / 32 * 32, 0);
        out.insert(out.end(), data.begin(), data.end());
        return out;
    }

private:
    using Entry = std::pair<std::string, std::vector<uint8_t>>;  // key, then its type and value as written
    struct Tensor {
        std::string name;
        std::vector<uint64_t> dims;
        uint32_t type;
        std::vector<uint8_t> data;
    };
    static void put(std::vector<uint8_t>& out, const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    }
    static void u32(std::vector<uint8_t>& out, uint32_t v) { put(out, &v, 4); }
    static void u64(std::vector<uint8_t>& out, uint64_t v) { put(out, &v, 8); }
    static void name(std::vector<uint8_t>& out, const std::string& s) {
        u64(out, s.size());
        out.insert(out.end(), s.begin(), s.end());
    }
    void scalar(const std::string& key, uint32_t type, const void* v, size_t n) {
        std::vector<uint8_t> body;
        u32(body, type);
        put(body, v, n);
        set(key, body);
    }
    void set(const std::string& key, const std::vector<uint8_t>& body) {
        for (Entry& e : meta_)
            if (e.first == key) {
                e.second = body;
                return;
            }
        meta_.push_back({key, body});
    }
    std::vector<Entry> meta_;
    std::vector<Tensor> tensors_;
};

// What a flag model declares besides its flag.
struct FlagOptions {
    int window = 64, stride = 48, min = 1;
    std::string profile = "none";
    int entities = 1;          // entity types of the tag head (only entity 0 is ever marked)
    bool choice_head = false;  // a second head: choice, pooling `last`
    float choice_gain = 0.1f;  // its weights; large ones overflow its logits to infinity (NaN probabilities)
};

// d_model 8, one ssm_v2 block that adds 0, a tag head that labels S-0 exactly the bytes `flag` accepts.
inline GgufBuilder flag_builder(const std::function<bool(uint8_t)>& flag, const FlagOptions& o = FlagOptions()) {
    constexpr int d = 8;
    GgufBuilder g;
    g.text("general.architecture", "purebyte");
    g.i32("purebyte.format_version", 3);
    g.i32("purebyte.d_model", d);
    g.i32("purebyte.n_layers", 1);
    g.i32("purebyte.n_heads", 1);
    g.i32("purebyte.d_head", 8);
    g.i32("purebyte.d_state", 8);
    g.i32("purebyte.n_groups", 1);
    g.i32("purebyte.conv_width", 0);
    std::vector<float> embed(256 * d, 0.f);
    for (int b = 0; b < 256; ++b) embed[static_cast<size_t>(b) * d + (flag(static_cast<uint8_t>(b)) ? 0 : 1)] = 4.f;
    g.tensor("embed.weight", {256, d}, embed);
    const int di = 8, gn = 8;
    g.tensor("blocks.0.norm.weight", {d}, std::vector<float>(d, 1.f));
    g.tensor("blocks.0.in_proj.weight", {2 * di + 2 * gn, d}, std::vector<float>((2 * di + 2 * gn) * d, 0.f));
    g.tensor("blocks.0.dt_proj.weight", {1, d}, std::vector<float>(d, 0.f));
    g.tensor("blocks.0.dt_bias", {1}, {0.f});
    g.tensor("blocks.0.A_log", {1}, {0.f});
    g.tensor("blocks.0.D", {1}, {0.f});
    g.tensor("blocks.0.gated_norm.weight", {di}, std::vector<float>(di, 1.f));
    g.tensor("blocks.0.out_proj.weight", {d, di}, std::vector<float>(d * di, 0.f));
    g.tensor("out_norm.weight", {d}, std::vector<float>(d, 1.f));
    g.i32("purebyte.head.count", o.choice_head ? 2 : 1);
    g.text("purebyte.head.0.type", "tag");
    const int L = 1 + 4 * o.entities;
    std::vector<float> weight(static_cast<size_t>(L) * d, 0.f), bias(static_cast<size_t>(L), -10.f);
    weight[0 * d + 1] = 5.f;  // O where the byte is not flagged
    weight[4 * d + 0] = 5.f;  // S-0 where it is
    bias[0] = 0.f;
    bias[4] = 0.f;
    g.tensor("heads.0.proj.weight", {static_cast<uint64_t>(L), d}, weight);
    g.tensor("heads.0.proj.bias", {static_cast<uint64_t>(L)}, bias);
    if (o.choice_head) {
        g.text("purebyte.head.1.type", "choice");
        g.text("purebyte.head.1.pooling", "last");
        g.tensor("heads.1.mlp.0.weight", {4, d}, std::vector<float>(4 * d, o.choice_gain));
        g.tensor("heads.1.mlp.0.bias", {4}, std::vector<float>(4, 0.f));
        g.tensor("heads.1.mlp.2.weight", {2, 4}, std::vector<float>(8, o.choice_gain));
        g.tensor("heads.1.mlp.2.bias", {2}, {0.f, 0.f});
    }
    g.i32("purebyte.window.size", o.window);
    g.i32("purebyte.window.stride", o.stride);
    g.i32("purebyte.window.min", o.min);
    g.text("purebyte.profile", o.profile);
    return g;
}

inline std::vector<uint8_t> flag_model(const std::function<bool(uint8_t)>& flag, const FlagOptions& o = FlagOptions()) {
    return flag_builder(flag, o).bytes();
}

inline bool is_upper(uint8_t b) { return b >= 'A' && b <= 'Z'; }
inline bool is_digit(uint8_t b) { return b >= '0' && b <= '9'; }

// Loads a model from memory: its status, and the message in `message`.
inline pb_status load_model(const std::vector<uint8_t>& file, pb_model** model, std::string* message = nullptr) {
    pb_error err;
    err.message[0] = '\0';
    *model = nullptr;
    const pb_status status = pb_model_load_memory(file.data(), file.size(), model, &err);
    if (message) *message = status == PB_OK ? "" : err.message;
    return status;
}

// True when `shown` holds no 4 consecutive characters of `value` other than its first 4 (a mask keeps at most the 4
// first and the 2 last characters of what it hides).
inline bool hidden_in(const std::string& value, const std::string& shown) {
    for (size_t i = 1; i + 4 < value.size(); ++i)
        if (shown.find(value.substr(i, 4)) != std::string::npos) return false;
    return true;
}

}  // namespace pbtest
