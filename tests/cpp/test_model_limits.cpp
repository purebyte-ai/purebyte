// Model files that declare more than they hold (format/gguf.h, model/ngram.h): tensors laid over the same bytes, and
// n-gram tables whose shapes, multiplied together, would size buffers wrongly; and files that declare a window whose
// cost has no relation to their size, or values that only look valid once narrowed, or contradictory keys. Each is
// refused when it is loaded, with a message that names the problem. The files are built here, in memory.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "crafted_models.h"
#include "purebyte/pb.h"
#include "test.h"

namespace {

using Bytes = std::vector<uint8_t>;

void put32(Bytes& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void put64(Bytes& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void put_string(Bytes& out, const std::string& s) {
    put64(out, s.size());
    out.insert(out.end(), s.begin(), s.end());
}

constexpr uint32_t kF32 = 0, kI8 = 24;

// A GGUF v3 file (spec/FORMAT.md, section 2) with zero-filled tensors.
class Gguf {
public:
    void integer(const std::string& key, int32_t v) {
        put_string(metadata_, key);
        put32(metadata_, 5);  // i32
        put32(metadata_, static_cast<uint32_t>(v));
        ++keys_;
    }
    void text(const std::string& key, const std::string& v) {
        put_string(metadata_, key);
        put32(metadata_, 8);  // string
        put_string(metadata_, v);
        ++keys_;
    }
    // A tensor with bytes of its own after the others'. `dims` in GGUF order: the fastest-varying dimension first.
    uint64_t tensor(const std::string& name, const std::vector<uint64_t>& dims, uint32_t type) {
        uint64_t bytes = type == kF32 ? 4 : 1;
        for (uint64_t d : dims) bytes *= d;
        const uint64_t offset = data_.size();
        data_.resize(static_cast<size_t>(offset + (bytes + 31) / 32 * 32), 0);
        tensor_at(name, dims, type, offset);
        return offset;
    }
    // A tensor at an offset of the data section chosen by the test.
    void tensor_at(const std::string& name, const std::vector<uint64_t>& dims, uint32_t type, uint64_t offset) {
        put_string(infos_, name);
        put32(infos_, static_cast<uint32_t>(dims.size()));
        for (uint64_t d : dims) put64(infos_, d);
        put32(infos_, type);
        put64(infos_, offset);
        ++tensors_;
    }
    Bytes bytes() const {
        Bytes out;
        put32(out, 0x46554747);  // "GGUF"
        put32(out, 3);
        put64(out, tensors_);
        put64(out, keys_);
        out.insert(out.end(), metadata_.begin(), metadata_.end());
        out.insert(out.end(), infos_.begin(), infos_.end());
        out.resize((out.size() + 31) / 32 * 32, 0);
        out.insert(out.end(), data_.begin(), data_.end());
        return out;
    }

private:
    Bytes metadata_, infos_, data_;
    uint64_t keys_ = 0, tensors_ = 0;
};

// The start of every model: what the loader reads before the n-gram tables.
Gguf model_start(int heads) {
    Gguf g;
    g.text("general.architecture", "purebyte");
    g.integer("purebyte.format_version", 3);
    g.integer("purebyte.d_model", 8);
    g.integer("purebyte.n_layers", 1);
    g.integer("purebyte.ngram.bits", 4);
    if (heads > 1) g.integer("purebyte.ngram.heads", heads);
    g.tensor("embed.weight", {8, 256}, kF32);
    return g;
}

std::string table(int order, int head, int heads, const char* what) {
    return "ngram." + std::to_string(order) + "." + (heads > 1 ? std::to_string(head) + "." : "") + what;
}

// Loads the file: its status, and the message in `message`.
pb_status load(const Bytes& file, std::string& message) {
    pb_model* model = nullptr;
    pb_error err;
    err.message[0] = '\0';
    const pb_status status = pb_model_load_memory(file.data(), file.size(), &model, &err);
    message = status == PB_OK ? "" : err.message;
    pb_model_free(model);
    return status;
}

}  // namespace

TEST(model_limits_tensors_that_share_bytes_are_refused) {
    // The shape of the crafted model of the report: 4096 4-bit tables of 2^19 + 1 bytes, all at the same offset (so
    // about 1 MB of file), whose widths multiplied together overflow a 32-bit int. The tables share their bytes.
    Gguf g = model_start(64);
    const uint64_t data = g.tensor("ngram.1.0.table", {(1u << 19) + 1, 1}, kI8);
    const uint64_t scale = g.tensor("ngram.1.0.scale", {1}, kF32);
    for (int order = 1; order <= 64; ++order)
        for (int head = 0; head < 64; ++head) {
            if (order == 1 && head == 0) continue;
            g.tensor_at(table(order, head, 64, "table"), {(1u << 19) + 1, 1}, kI8, data);
            g.tensor_at(table(order, head, 64, "scale"), {1}, kF32, scale);
        }
    g.tensor("ngram.proj.weight", {8192, 8}, kF32);
    const Bytes file = g.bytes();
    CHECK(file.size() < (2u << 20));
    std::string message;
    CHECK(load(file, message) == PB_ERR_FORMAT);
    CHECK_MSG(message.find("overlap") != std::string::npos, message);

    // Two small tensors with their last 32 bytes in common (offsets stay aligned).
    Gguf two = model_start(1);
    const uint64_t at = two.tensor("ngram.1.table", {32, 16}, kI8);
    two.tensor_at("ngram.1.scale", {8}, kF32, at + 32 * 16 - 32);
    CHECK(load(two.bytes(), message) == PB_ERR_FORMAT);
    CHECK_MSG(message.find("`ngram.1.table` and `ngram.1.scale` overlap") != std::string::npos, message);
}

TEST(model_limits_ngram_shapes_are_bounded) {
    std::string message;

    // One table of the crafted model's shape, bytes of its own: 2^20 + 2 columns.
    Gguf wide = model_start(1);
    wide.tensor("ngram.1.table", {(1u << 19) + 1, 1}, kI8);
    wide.tensor("ngram.1.scale", {1}, kF32);
    CHECK(load(wide.bytes(), message) == PB_ERR_FORMAT);
    CHECK_MSG(message.find("has 1 buckets of 1048578 columns; this runtime reads at most") != std::string::npos,
              message);

    // Small tables whose concatenation is too wide: 2 orders x 64 heads x 1024 columns.
    Gguf many = model_start(64);
    for (int order = 1; order <= 2; ++order)
        for (int head = 0; head < 64; ++head) {
            many.tensor(table(order, head, 64, "table"), {512, 1}, kI8);
            many.tensor(table(order, head, 64, "scale"), {1}, kF32);
        }
    CHECK(load(many.bytes(), message) == PB_ERR_FORMAT);
    CHECK_MSG(message.find("add up to 131072 columns (2 orders x 64 heads x 1024)") != std::string::npos, message);

    // Too many buckets (16 MB of table).
    Gguf deep = model_start(1);
    deep.tensor("ngram.1.table", {1, (1u << 24) + 1}, kI8);
    CHECK(load(deep.bytes(), message) == PB_ERR_FORMAT);
    CHECK_MSG(message.find("has 16777217 buckets") != std::string::npos, message);

    // At the limits, the tables are accepted (the file then fails further on, on its missing blocks).
    Gguf edge = model_start(1);
    edge.tensor("ngram.1.table", {1u << 15, 1}, kI8);
    edge.tensor("ngram.1.scale", {1}, kF32);
    edge.tensor("ngram.proj.weight", {1u << 16, 8}, kF32);
    CHECK(load(edge.bytes(), message) != PB_OK);
    CHECK_MSG(message.find("n-gram") == std::string::npos && message.find("overlap") == std::string::npos, message);
}

namespace {

using pbtest::GgufBuilder;

// The status of loading `g`, and its message in `message`.
pb_status load_built(const GgufBuilder& g, std::string& message) {
    pb_model* model = nullptr;
    const pb_status status = pbtest::load_model(g.bytes(), &model, &message);
    pb_model_free(model);
    return status;
}

GgufBuilder flag() { return pbtest::flag_builder(pbtest::is_upper); }

// A choice head at index `i` named `name` (empty: no name key), pooling `last`.
void add_choice(GgufBuilder& g, int i, const std::string& name) {
    const std::string p = "heads." + std::to_string(i) + ".", k = "purebyte.head." + std::to_string(i) + ".";
    g.text(k + "type", "choice");
    g.text(k + "pooling", "last");
    if (!name.empty()) g.text(k + "name", name);
    g.tensor(p + "mlp.0.weight", {4, 8}, std::vector<float>(32, 0.1f));
    g.tensor(p + "mlp.0.bias", {4}, std::vector<float>(4, 0.f));
    g.tensor(p + "mlp.2.weight", {2, 4}, std::vector<float>(8, 0.1f));
    g.tensor(p + "mlp.2.bias", {2}, {0.f, 0.f});
}

}  // namespace

TEST(model_limits_non_finite_values_are_refused) {
    // NaN or infinity in a tensor or in a number of the metadata: a corrupt file, whose outputs would only be NaN and
    // whose ordinal thresholds could not even be sorted (review R1, findings 2 and 15).
    std::string message;
    CHECK(load_built(flag(), message) == PB_OK);
    GgufBuilder nan_bias = flag();
    std::vector<float> bias(5, -10.f);
    bias[2] = NAN;
    nan_bias.tensor("heads.0.proj.bias", {5}, bias);
    CHECK(load_built(nan_bias, message) == PB_ERR_FORMAT);
    CHECK_MSG(message.find("`heads.0.proj.bias` holds a NaN or infinite value (element 2)") != std::string::npos,
              message);
    GgufBuilder inf_embedding = flag();
    std::vector<float> embed(256 * 8, 0.f);
    embed[77] = INFINITY;
    inf_embedding.tensor("embed.weight", {256, 8}, embed);
    CHECK(load_built(inf_embedding, message) == PB_ERR_FORMAT);
    GgufBuilder ordinal = flag();
    ordinal.i32("purebyte.head.count", 2);
    ordinal.text("purebyte.head.1.type", "ordinal");
    ordinal.tensor("heads.1.w.weight", {1, 16}, std::vector<float>(16, 0.01f));
    ordinal.tensor("heads.1.b", {3}, {0.5f, NAN, -0.5f});
    CHECK(load_built(ordinal, message) == PB_ERR_FORMAT);
    for (const char* key : {"purebyte.head.0.operating_bias", "purebyte.head.1.temperature"}) {
        GgufBuilder g = flag();
        g.i32("purebyte.head.count", 2);
        add_choice(g, 1, "");
        g.f32(key, INFINITY);
        CHECK(load_built(g, message) == PB_ERR_FORMAT);
        CHECK_MSG(message.find(std::string("`") + key + "` must be a finite number") != std::string::npos, message);
    }
}

TEST(model_limits_values_are_checked_before_narrowing) {
    // 64-bit values that only look valid once narrowed to 32 bits are refused (review R1, finding 12).
    std::string message;
    GgufBuilder version = flag();
    version.i64("purebyte.format_version", (int64_t(1) << 32) + 3);
    CHECK(load_built(version, message) == PB_ERR_UNSUPPORTED);
    CHECK_MSG(message.find("model format version 4294967299") != std::string::npos, message);
    for (int64_t gate : {int64_t(1) << 32, int64_t(-7), int64_t(64), int64_t(1)}) {
        GgufBuilder g = flag();
        g.i32("purebyte.head.count", 2);
        add_choice(g, 1, "");
        g.i64("purebyte.head.0.gated_by", gate);
        CHECK_MSG(load_built(g, message) == (gate == 1 ? PB_OK : PB_ERR_FORMAT), std::to_string(gate) + ": " + message);
    }
    GgufBuilder group = flag();
    group.i64("purebyte.tern.group", (int64_t(1) << 32) + 8);
    CHECK(load_built(group, message) == PB_ERR_FORMAT);
    CHECK_MSG(message.find("purebyte.tern.group") != std::string::npos, message);
}

TEST(model_limits_head_names_are_made_unique) {
    // "c", "c_3", "c": the third takes "_3" once, which the second has, and again (review R1, finding 13).
    GgufBuilder g = flag();
    g.i32("purebyte.head.count", 4);
    add_choice(g, 1, "c");
    add_choice(g, 2, "c_3");
    add_choice(g, 3, "c");
    pb_model* model = nullptr;
    std::string message;
    CHECK_MSG(pbtest::load_model(g.bytes(), &model, &message) == PB_OK, message);
    if (!model) return;
    const std::string description = pb_model_describe(model);
    for (const char* name : {"\"name\":\"tag\"", "\"name\":\"c\"", "\"name\":\"c_3\"", "\"name\":\"c_3_3\""})
        CHECK_MSG(description.find(name) != std::string::npos, description);
    pb_model_free(model);
}

TEST(model_limits_one_window_costs_a_bounded_amount) {
    // What one window of the declared size costs is bounded (spec/FORMAT.md, section 13; review R1, finding 6): the
    // declared window, window x the widest row per position, the scan of an ssm_v2 block, the tag head's entity types,
    // and the window of a model with attention (T^2 work). A small file must not declare gigabytes or hours.
    std::string message;
    GgufBuilder window = flag();
    window.i32("purebyte.window.size", (1 << 20) + 1);
    CHECK(load_built(window, message) == PB_ERR_FORMAT);
    CHECK_MSG(message.find("purebyte.window.size") != std::string::npos, message);
    GgufBuilder rows = flag();  // in_proj rows of 32 floats: 2^20 x 32 > 2^24
    rows.i32("purebyte.window.size", 1 << 20);
    CHECK(load_built(rows, message) == PB_ERR_FORMAT);
    CHECK_MSG(message.find("widest row") != std::string::npos, message);
    GgufBuilder scan = flag();  // one head of 4096 channels and 4096 states per position
    scan.i32("purebyte.d_head", 4096);
    scan.i32("purebyte.d_state", 4096);
    CHECK(load_built(scan, message) == PB_ERR_FORMAT);
    CHECK_MSG(message.find("a scan of 16777216 floats per position") != std::string::npos, message);
    for (int entities : {256, 257}) {  // the reviewer's file had 2,000 entity types: 21 s for one 64-byte window
        GgufBuilder tag = pbtest::flag_builder(pbtest::is_upper, [&] {
            pbtest::FlagOptions o;
            o.entities = entities;
            return o;
        }());
        CHECK_MSG(load_built(tag, message) == (entities == 256 ? PB_OK : PB_ERR_FORMAT), message);
    }
    // Attention: the block of flag() replaced by an attention block with heads of 8.
    GgufBuilder attention = flag();
    for (const char* t : {"blocks.0.in_proj.weight", "blocks.0.dt_proj.weight", "blocks.0.dt_bias", "blocks.0.A_log",
                          "blocks.0.D", "blocks.0.gated_norm.weight", "blocks.0.out_proj.weight"})
        attention.drop_tensor(t);
    attention.text("purebyte.block.0.type", "attention");
    attention.i32("purebyte.block.0.head_dim", 8);
    attention.tensor("blocks.0.qkv.weight", {24, 8}, std::vector<float>(24 * 8, 0.f));
    attention.tensor("blocks.0.out.weight", {8, 8}, std::vector<float>(64, 0.f));
    CHECK_MSG(load_built(attention, message) == PB_OK, message);
    attention.i32("purebyte.window.size", (1 << 13) + 1);
    CHECK(load_built(attention, message) == PB_ERR_FORMAT);
    CHECK_MSG(message.find("attention") != std::string::npos, message);
}

TEST(model_limits_every_window_evaluates_document_bytes) {
    // A shortest window longer than the window, a stride longer than the document part of a query model's window:
    // the model would evaluate nothing, or skip bytes (review R1, findings 7 and 14).
    std::string message;
    GgufBuilder min = flag();
    min.i32("purebyte.window.min", 1000);
    CHECK(load_built(min, message) == PB_ERR_FORMAT);
    CHECK_MSG(message.find("purebyte.window.min") != std::string::npos, message);
    GgufBuilder query = flag();
    query.i32("purebyte.window.size", 96);
    query.i32("purebyte.query.region", 48);
    query.i32("purebyte.query.max", 3);
    query.i32("purebyte.window.stride", 64);
    CHECK(load_built(query, message) == PB_ERR_FORMAT);
    CHECK_MSG(message.find("document part") != std::string::npos, message);
    // Without a stride, the default walks the document part: every document byte is in a window.
    query.drop_key("purebyte.window.stride");
    pb_model* model = nullptr;
    CHECK_MSG(pbtest::load_model(query.bytes(), &model, &message) == PB_OK, message);
    pb_session* session = nullptr;
    pb_error err;
    CHECK(pb_session_create(nullptr, &session, &err) == PB_OK);
    if (!model || !session) return;
    const std::string text(300, 'a');
    const pb_input input{reinterpret_cast<const uint8_t*>(text.data()), text.size()};
    const char* queries[] = {"user"};
    pb_scan_options so;
    pb_scan_options_init(&so);
    so.queries = queries;
    so.query_count = 1;
    pb_scan_result* result = nullptr;
    CHECK_MSG(pb_scan(session, model, &input, 1, &so, &result, &err) == PB_OK, err.message);
    const pb_window* windows = nullptr;
    const size_t n = pb_scan_result_windows(result, 0, &windows);
    int64_t covered = 0;
    for (size_t w = 0; w < n; ++w) {
        CHECK(windows[w].start <= covered);  // no gap
        covered = std::max<int64_t>(covered, windows[w].start + windows[w].length);
    }
    CHECK(covered == 300);
    pb_scan_result_free(result);
    // A caller window that leaves fewer document bytes than the shortest window, or a stride longer than them.
    query.i32("purebyte.window.min", 24);
    pb_model_free(model);
    CHECK_MSG(pbtest::load_model(query.bytes(), &model, &message) == PB_OK, message);
    so.window = 64;  // 16 document bytes
    CHECK(pb_scan(session, model, &input, 1, &so, &result, &err) == PB_ERR_ARGUMENT);
    so.window = 96;
    so.stride = 49;  // 48 document bytes
    CHECK(pb_scan(session, model, &input, 1, &so, &result, &err) == PB_ERR_ARGUMENT);
    pb_session_free(session);
    pb_model_free(model);
}

TEST(model_limits_one_tag_head) {
    // A second tag head would run and never be reported: the result has one list of spans (review R1, finding 25).
    GgufBuilder g = flag();
    g.i32("purebyte.head.count", 2);
    g.text("purebyte.head.1.type", "tag");
    g.tensor("heads.1.proj.weight", {5, 8}, std::vector<float>(40, 0.f));
    g.tensor("heads.1.proj.bias", {5}, std::vector<float>(5, 0.f));
    std::string message;
    CHECK(load_built(g, message) == PB_ERR_FORMAT);
    CHECK_MSG(message.find("second tag head") != std::string::npos, message);
}

TEST(model_limits_messages_are_printable) {
    // Names read from a file reach the caller's message with every control character escaped (review R1, finding 35).
    GgufBuilder g = flag();
    g.text("purebyte.head.0.type", "tag\x1b[31m\x9b\xff");
    std::string message;
    CHECK(load_built(g, message) == PB_ERR_UNSUPPORTED);
    CHECK_MSG(message.find('\x1b') == std::string::npos && message.find("tag\\x1b[31m") != std::string::npos, message);
    for (const unsigned char c : message) CHECK(c >= 0x20 && c != 0x7f);
}
