// Head type `tag`: typed spans over the window's bytes. A linear map gives per-byte BIOES logits for k entity types
// (L = 1 + 4k labels), log-softmax turns them into log-probabilities, and bioes::viterbi decodes the best valid
// sequence with the operating bias: the user's (per type or scalar), else the file's per-type vector
// (`operating_bias_vec`), else the file's scalar (`operating_bias`). Label O is never biased.
//   heads.<i>.proj.weight [L, d_model], heads.<i>.proj.bias [L]
#include <cmath>
#include <vector>

#include "core/failure.h"
#include "heads/bioes.h"
#include "heads/head.h"

namespace pb {

namespace {

constexpr int kMaxEntities = 256;

class TagHead final : public Head {
public:
    TagHead(LoadContext& ctx, int index) : Head(ctx, index, "tag") {
        d_ = ctx.d_model();
        const std::string p = "heads." + std::to_string(index) + ".";
        const gguf::Tensor& w = ctx.tensor(p + "proj.weight");
        // Dimensions are checked as the file's 64-bit values before they are narrowed. The entity types are bounded:
        // decoding costs time in proportion to them at every position (spec/FORMAT.md, section 11.6).
        const uint64_t rows = w.dim(1);
        if (w.type != gguf::TensorType::F32 || w.dims.size() != 2 || w.dim(0) != static_cast<uint64_t>(d_) ||
            rows < 5 || (rows - 1) % 4 != 0 || rows > 1 + 4 * static_cast<uint64_t>(kMaxEntities))
            fail(PB_ERR_FORMAT, format("`%sproj.weight` has shape %s, expected [1 + 4k, %d] for k = 1..%d entity types",
                                       p.c_str(), w.shape().c_str(), d_, kMaxEntities));
        L_ = static_cast<int>(rows);
        weight_ = w.f32();
        bias_ = ctx.f32(p + "proj.bias", {L_});
        entities_ = (L_ - 1) / 4;
        const std::string key_entities = LoadContext::key("head", index, "n_entities");
        const std::string key_labels = LoadContext::key("head", index, "n_labels");
        if (file_.integer(key_entities, entities_) != entities_ || file_.integer(key_labels, L_) != L_)
            fail(PB_ERR_FORMAT,
                 format("head %d: n_labels / n_entities contradict the %d rows of its projection", index, L_));
        const std::string scheme = file_.text(LoadContext::key("head", index, "scheme"), "BIOES:O,B-e,I-e,E-e,S-e");
        if (scheme != "BIOES:O,B-e,I-e,E-e,S-e")
            fail(PB_ERR_UNSUPPORTED,
                 format("head %d: label scheme `%s` (known: BIOES:O,B-e,I-e,E-e,S-e)", index, scheme.c_str()));
        if (!labels_.empty() && static_cast<int>(labels_.size()) != entities_)
            fail(PB_ERR_FORMAT, format("head %d names %zu entity types and has %d", index, labels_.size(), entities_));
        if (labels_.empty())
            for (int e = 0; e < entities_; ++e) labels_.push_back("entity_" + std::to_string(e));
        operating_bias_ = checked_finite(file_, LoadContext::key("head", index, "operating_bias"), 0.0);
        operating_bias_vec_ = checked_finite_list(file_, LoadContext::key("head", index, "operating_bias_vec"));
        if (!operating_bias_vec_.empty() && static_cast<int>(operating_bias_vec_.size()) != entities_)
            fail(PB_ERR_FORMAT, format("head %d has %d entity types and %zu per-type biases", index, entities_,
                                       operating_bias_vec_.size()));
    }

    const char* type() const override { return "tag"; }
    int entities() const { return entities_; }

    // The per-label bias of one call (see the file comment).
    std::vector<float> label_bias(const SpanBias& user) const {
        if (!user.per_type.empty() && static_cast<int>(user.per_type.size()) != entities_)
            fail(PB_ERR_ARGUMENT,
                 format("%zu per-type biases for a head with %d entity types", user.per_type.size(), entities_));
        std::vector<float> b(static_cast<size_t>(L_), 0.f);
        for (int e = 0; e < entities_; ++e) {
            float v = operating_bias_;
            if (!user.per_type.empty())
                v = user.per_type[e];
            else if (user.has_scalar)
                v = user.scalar;
            else if (!operating_bias_vec_.empty())
                v = operating_bias_vec_[e];
            for (int k = 1 + 4 * e; k < 5 + 4 * e; ++k) b[k] = v;
        }
        return b;
    }

    void run(const HeadInput& in, HeadOutput& out) const override {
        const int T = in.T;
        std::vector<float> logp(static_cast<size_t>(T) * L_);
        for (int t = 0; t < T; ++t) {
            const float* h = in.hidden + static_cast<size_t>(t) * d_;
            float* o = logp.data() + static_cast<size_t>(t) * L_;
            float top = -1e30f;
            for (int k = 0; k < L_; ++k) {
                float a = bias_[k];
                const float* w = weight_ + static_cast<size_t>(k) * d_;
                for (int i = 0; i < d_; ++i) a += w[i] * h[i];
                o[k] = a;
                top = std::fmax(top, a);
            }
            float sum = 0.f;
            for (int k = 0; k < L_; ++k) sum += std::exp(o[k] - top);
            const float lse = top + std::log(sum);
            for (int k = 0; k < L_; ++k) o[k] -= lse;
        }
        const std::vector<float> bias = label_bias(in.bias);
        std::vector<int> path;
        bioes::viterbi(logp.data(), T, L_, entities_, bias.data(), path);
        bioes::spans(path, logp.data(), L_, out.spans);
        if (in.first_position > 0) {  // spans inside a query prefix are not part of the document
            std::vector<Span> kept;
            for (Span s : out.spans) {
                if (s.end <= in.first_position) continue;
                if (s.start < in.first_position) s.start = in.first_position;
                kept.push_back(s);
            }
            out.spans.swap(kept);
        }
        out.computed = true;
    }

    std::string describe() const override {
        json::Writer w = describe_begin();
        w.field("entities", entities_).field("operating_bias", static_cast<double>(operating_bias_));
        if (!operating_bias_vec_.empty()) {
            w.key("operating_bias_vec").begin_array();
            for (float v : operating_bias_vec_) w.real(v);
            w.end_array();
        }
        w.end_object();
        return w.take();
    }

private:
    int d_ = 0, L_ = 0, entities_ = 0;
    const float* weight_ = nullptr;
    const float* bias_ = nullptr;
    float operating_bias_ = 0.f;
    std::vector<float> operating_bias_vec_;
};

}  // namespace

std::unique_ptr<Head> load_tag_head(LoadContext& ctx, int index) { return std::make_unique<TagHead>(ctx, index); }

}  // namespace pb
