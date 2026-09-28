// Assembles a Model from a GGUF file: global checks, then every declared part through its registry, then the check
// that every tensor of the file was used (anything left over would be silently ignored otherwise).
#include "model/model.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "core/failure.h"
#include "core/json.h"

namespace pb {

namespace {

// Optional capabilities a file may require (`purebyte.required_features`). A reader refuses a file that requires
// one it does not know, instead of computing something else.
const char* const kKnownFeatures[] = {"attention",   "lookahead",  "ngram_gate", "ngram_layer",
                                      "ngram_heads", "multilabel", "score",      "ordinal",
                                      "byte_map",    "query",      "stream"};

void check_container(const gguf::File& f, int& version) {
    const std::string arch = f.text("general.architecture", "");
    if (arch != "purebyte")
        fail(PB_ERR_FORMAT, "not a PureByte model (general.architecture is `" + arch + "`, expected `purebyte`)");
    // Compared as the 64-bit value of the file: a version that only looks valid once narrowed is still refused.
    const int64_t declared = f.integer("purebyte.format_version", -1);
    if (declared < 2) fail(PB_ERR_FORMAT, "`purebyte.format_version` is missing or older than 2");
    if (declared > kFormatVersion)
        fail(PB_ERR_UNSUPPORTED, format("model format version %lld; this library reads up to version %d: update "
                                        "purebyte",
                                        static_cast<long long>(declared), kFormatVersion));
    version = static_cast<int>(declared);
    for (const std::string& feature : f.texts("purebyte.required_features")) {
        bool known = false;
        for (const char* k : kKnownFeatures) known = known || feature == k;
        if (!known)
            fail(PB_ERR_UNSUPPORTED, "the model requires the feature `" + feature +
                                         "`, which this version of purebyte "
                                         "does not implement: update purebyte");
    }
    if (f.text("purebyte.tern.type", "pb_tern2") != "pb_tern2")
        fail(PB_ERR_UNSUPPORTED, "ternary type `" + f.text("purebyte.tern.type") + "` (known: pb_tern2)");
    if (f.text("purebyte.tern.encoding", "00=0,01=+1,10=-1") != "00=0,01=+1,10=-1")
        fail(PB_ERR_UNSUPPORTED,
             "ternary encoding `" + f.text("purebyte.tern.encoding") + "` (known: 00=0,01=+1,10=-1)");
}

// No F32 tensor may hold a NaN or an infinity: such a value is a corrupt file, it would only produce NaN outputs, and
// some loaders order values (ordinal thresholds) with comparisons that NaN breaks. Checked before any part is loaded.
void check_finite_tensors(const gguf::File& f) {
    for (const gguf::Tensor& t : f.tensors()) {
        if (t.type != gguf::TensorType::F32) continue;
        const float* v = t.f32();
        for (uint64_t k = 0; k < t.count; ++k)
            if (!std::isfinite(v[k]))
                fail(PB_ERR_FORMAT, format("tensor `%s` holds a NaN or infinite value (element %llu): the file is "
                                           "corrupt",
                                           t.name.c_str(), static_cast<unsigned long long>(k)));
    }
}

void load_heads(LoadContext& ctx, Model& m) {
    const gguf::File& f = ctx.file();
    // A file without `purebyte.head.count` has one head, `heads.0`, of type choice (the oldest exports).
    const int count = static_cast<int>(checked_int(f, "purebyte.head.count", 0, 64, 1));
    for (int i = 0; i < count; ++i) {
        const std::string type = f.text(LoadContext::key("head", i, "type"), "choice");
        const HeadType* t = find_head_type(type);
        if (!t)
            fail(PB_ERR_UNSUPPORTED,
                 format("head %d is of type `%s`, which this version of purebyte does not implement "
                        "(known: %s; model format %d): update purebyte",
                        i, type.c_str(), known_head_types().c_str(), m.format_version));
        m.heads.push_back(t->load(ctx, i));
    }
    // Results are keyed by head name: a repeated name gets "_<i>" appended (i = the head's index), again until it is
    // unique ("tag", "tag_2", "tag" -> "tag", "tag_2", "tag_2_2").
    std::set<std::string> names;
    int tag_heads = 0;
    for (auto& h : m.heads) {
        std::string name = h->name();
        while (names.count(name)) name += "_" + std::to_string(h->index());
        names.insert(name);
        if (name != h->name()) h->set_name(name);
        if (auto* exit = dynamic_cast<const ExitHead*>(h.get())) {
            if (m.exit_head) fail(PB_ERR_FORMAT, "the model declares more than one exit head");
            m.exit_head = exit;
        }
        if (m.first_choice_head < 0 && std::string(h->type()) == "choice") m.first_choice_head = h->index();
        // Spans have one list per result (spec/OUTPUT.md): a second tag head's spans would be computed and never
        // reported.
        if (std::string(h->type()) == "tag" && ++tag_heads > 1)
            fail(PB_ERR_FORMAT, format("head %d is a second tag head; a model has at most one", h->index()));
    }
    for (const auto& h : m.heads) {
        const int g = h->gated_by();
        if (g < 0) continue;
        if (g >= count || g == h->index() || !m.heads[g]->can_gate() || m.heads[g]->gated_by() >= 0)
            fail(PB_ERR_FORMAT,
                 format("head %d is gated by head %d, which is not an ungated choice, multilabel or ordinal "
                        "head",
                        h->index(), g));
    }
}

void load_window(const gguf::File& f, Model& m) {
    m.window.size = static_cast<int>(checked_int(f, "purebyte.window.size", 1, kMaxWindowSize, 512));
    const std::string context = f.text("purebyte.context", "window");
    if (context != "window" && context != "stream")
        fail(PB_ERR_UNSUPPORTED, "`purebyte.context` = `" + context + "` (known: window, stream)");
    m.streaming = context == "stream";
    m.profile = f.text("purebyte.profile", "");
    if (f.has("purebyte.query.region")) {
        QueryTemplate& q = m.query;
        q.region = static_cast<int>(checked_int(f, "purebyte.query.region", 1, 1 << 20));
        q.max_queries = static_cast<int>(checked_int(f, "purebyte.query.max", 1, 1024));
        q.prefix = f.text("purebyte.query.prefix", "?");
        q.suffix = f.text("purebyte.query.suffix", "\n");
        const std::string pad = f.text("purebyte.query.pad", "\n");
        if (pad.size() != 1) fail(PB_ERR_FORMAT, "`purebyte.query.pad` must be one byte");
        q.pad = static_cast<uint8_t>(pad[0]);
        if (q.region >= m.window.size)
            fail(PB_ERR_FORMAT, "the query region does not leave room for the document in a window");
    }
    // Windows walk the document by the stride, and each holds `span` document bytes: the whole window, or what the
    // query region leaves. A stride beyond it would skip bytes, and a shortest window beyond it would evaluate none.
    // (In the stream context `min` bounds the whole input, not a chunk: a larger value is meaningful there.)
    const int span = m.window.document_bytes(m.query.region);
    const char* part =
        m.query.enabled() ? "the document part of a window (window.size - query.region)" : "`purebyte.window.size`";
    m.window.stride = static_cast<int>(checked_int(f, "purebyte.window.stride", 1, kMaxWindowSize, span - span / 4));
    const int default_min = m.streaming ? 24 : std::min(24, span);
    m.window.min_length = static_cast<int>(checked_int(f, "purebyte.window.min", 1, 1 << 24, default_min));
    if (m.window.stride > span)
        fail(PB_ERR_FORMAT, format("`purebyte.window.stride` (%d) is larger than %s (%d): bytes between windows would "
                                   "never be seen",
                                   m.window.stride, part, span));
    if (!m.streaming && m.window.min_length > span)
        fail(PB_ERR_FORMAT, format("`purebyte.window.min` (%d) is larger than %s (%d): no window would ever be "
                                   "evaluated",
                                   m.window.min_length, part, span));
}

// What one window of the declared size costs (spec/FORMAT.md, section 13). Per position it allocates the widest row
// of any block (and of the tag head's labels, and the residual stream); attention also does T^2 work. A small file
// must not be able to declare a window that needs gigabytes or runs for hours.
void check_window_cost(const Model& m) {
    int64_t width = m.d_model;
    bool attention = false;
    for (const auto& b : m.blocks) {
        width = std::max(width, static_cast<int64_t>(b->position_floats()));
        attention = attention || std::string(b->type()) == "attention";
    }
    for (const auto& h : m.heads)
        if (std::string(h->type()) == "tag")
            width = std::max(width, 1 + 4 * static_cast<int64_t>(h->labels().size()));  // labels: its entity types
    const int64_t floats = static_cast<int64_t>(m.window.size) * width;
    if (floats > kMaxWindowWidthFloats)
        fail(PB_ERR_FORMAT, format("a window of %d bytes with rows of %lld floats per position needs %lld floats; "
                                   "this runtime reads at most %lld (window.size x the widest row)",
                                   m.window.size, static_cast<long long>(width), static_cast<long long>(floats),
                                   static_cast<long long>(kMaxWindowWidthFloats)));
    if (attention && m.window.size > kMaxAttentionWindow)
        fail(PB_ERR_FORMAT,
             format("`purebyte.window.size` = %d in a model with attention blocks; this runtime reads at "
                    "most %d (attention does work in the square of the window)",
                    m.window.size, kMaxAttentionWindow));
}

std::string describe(const Model& m) {
    json::Writer w;
    w.begin_object()
        .field("format_version", m.format_version)
        .field("name", m.name)
        .field("d_model", m.d_model)
        .field("layers", static_cast<int>(m.blocks.size()));
    w.key("blocks").begin_array();
    for (const auto& b : m.blocks) w.raw(b->describe());
    w.end_array();
    if (m.ngram) w.key("ngram").raw(m.ngram->describe());
    if (m.lookahead) w.key("lookahead").raw(m.lookahead->describe());
    w.key("heads").begin_array();
    for (const auto& h : m.heads) w.raw(h->describe());
    w.end_array();
    w.key("window")
        .begin_object()
        .field("size", m.window.size)
        .field("stride", m.window.stride)
        .field("min", m.window.min_length)
        .end_object();
    w.field("context", m.streaming ? "stream" : "window");
    if (m.query.enabled())
        w.key("query").begin_object().field("region", m.query.region).field("max", m.query.max_queries).end_object();
    if (!m.profile.empty()) w.field("profile", m.profile);
    w.end_object();
    return w.take();
}

std::shared_ptr<const Model> assemble(std::shared_ptr<const gguf::File> file) {
    auto m = std::make_shared<Model>();
    m->file = file;
    const gguf::File& f = *file;
    check_container(f, m->format_version);
    check_finite_tensors(f);
    m->name = f.text("general.name", "");
    m->d_model = static_cast<int>(checked_int(f, "purebyte.d_model", 1, 1 << 16));
    const int n_layers = static_cast<int>(checked_int(f, "purebyte.n_layers", 1, 4096));

    gguf::TensorClaims claims(f);
    LoadContext ctx(f, claims, m->d_model);
    m->embedding = ctx.f32("embed.weight", {256, m->d_model});
    m->ngram = NGramInput::load(ctx, n_layers);
    for (int i = 0; i < n_layers; ++i) {
        const std::string type = f.text(LoadContext::key("block", i, "type"), "ssm_v2");
        const BlockType* t = find_block_type(type);
        if (!t)
            fail(PB_ERR_UNSUPPORTED,
                 format("block %d is of type `%s`, which this version of purebyte does not implement "
                        "(known: %s; model format %d): update purebyte",
                        i, type.c_str(), known_block_types().c_str(), m->format_version));
        m->blocks.push_back(t->load(ctx, i));
    }
    m->final_norm = ctx.f32("out_norm.weight", {m->d_model});
    m->lookahead = Lookahead::load(ctx);
    load_heads(ctx, *m);
    load_window(f, *m);
    check_window_cost(*m);

    const std::vector<std::string> leftovers = claims.unclaimed();
    if (!leftovers.empty())
        fail(PB_ERR_UNSUPPORTED,
             format("the file carries %zu tensor(s) this version of purebyte does not use (the first "
                    "is `%s`): it was exported for a newer runtime, and running it would compute "
                    "a different function",
                    leftovers.size(), leftovers[0].c_str()));
    if (m->streaming) {
        for (const auto& b : m->blocks)
            if (b->stream_state_floats() == 0)
                fail(PB_ERR_FORMAT,
                     format("the model declares the stream context, but block %d (%s) needs the whole window",
                            static_cast<int>(&b - &m->blocks[0]), b->type()));
        if (m->lookahead)
            fail(PB_ERR_FORMAT,
                 "the model declares the stream context and a lookahead, which reads ahead of the stream");
        if (m->query.enabled()) fail(PB_ERR_FORMAT, "the model declares the stream context and a query template");
        if (m->exit_head)
            fail(PB_ERR_FORMAT, "the model declares the stream context and an exit head, which needs windows");
    }
    m->description = describe(*m);
    return m;
}

}  // namespace

std::vector<uint8_t> QueryTemplate::encode(const std::vector<std::string>& queries) const {
    if (static_cast<int>(queries.size()) > max_queries)
        fail(PB_ERR_ARGUMENT, format("%zu queries; the model takes at most %d", queries.size(), max_queries));
    std::string text;
    for (const std::string& q : queries) text += prefix + q + suffix;
    if (static_cast<int>(text.size()) > region)
        fail(PB_ERR_ARGUMENT,
             format("the queries take %zu bytes; the model's query region holds %d", text.size(), region));
    std::vector<uint8_t> out(text.begin(), text.end());
    out.resize(static_cast<size_t>(region), pad);
    return out;
}

size_t Model::scratch_floats(int T) const {
    size_t n = 0;
    for (const auto& b : blocks) n = std::max(n, b->scratch_floats(T));
    return n;
}

size_t Model::member_scratch_floats(int T) const {
    size_t n = ngram ? ngram->member_scratch_floats() : 0;
    for (const auto& b : blocks) n = std::max(n, b->member_scratch_floats(T));
    return n;
}

size_t Model::stream_state_floats() const {
    size_t n = 0;
    for (const auto& b : blocks) {
        const size_t s = b->stream_state_floats();
        if (s == 0) return 0;
        n += s;
    }
    return lookahead ? 0 : n;
}

int Model::find_head(const std::string& type) const {
    for (const auto& h : heads)
        if (type == h->type()) return h->index();
    return -1;
}

std::shared_ptr<const Model> load_model(const std::string& path) { return assemble(gguf::File::open(path)); }

std::shared_ptr<const Model> load_model_memory(const void* data, size_t size) {
    return assemble(gguf::File::from_memory(data, size));
}

}  // namespace pb
