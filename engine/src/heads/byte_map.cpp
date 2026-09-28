// Head type `byte_map`: one class per byte (segmentation: which bytes are code, prose, base64, a key...). A linear map
// gives C logits per position and the byte takes the argmax (lowest index on ties); at most 256 classes.
//   heads.<i>.proj.weight [C, d_model], heads.<i>.proj.bias [C]
#include <vector>

#include "core/failure.h"
#include "heads/head.h"

namespace pb {

namespace {

class ByteMapHead final : public Head {
public:
    ByteMapHead(LoadContext& ctx, int index) : Head(ctx, index, "byte_map") {
        d_ = ctx.d_model();
        const std::string p = "heads." + std::to_string(index) + ".";
        const gguf::Tensor& w = ctx.tensor(p + "proj.weight");
        // 64-bit dimensions are checked before they are narrowed.
        if (w.type != gguf::TensorType::F32 || w.dims.size() != 2 || w.dim(0) != static_cast<uint64_t>(d_) ||
            w.dim(1) < 2 || w.dim(1) > 256)
            fail(PB_ERR_FORMAT, format("`%sproj.weight` has shape %s, expected [classes (2..256), %d]", p.c_str(),
                                       w.shape().c_str(), d_));
        classes_ = static_cast<int>(w.dim(1));
        weight_ = w.f32();
        bias_ = ctx.f32(p + "proj.bias", {classes_});
        if (!labels_.empty() && static_cast<int>(labels_.size()) != classes_)
            fail(PB_ERR_FORMAT, format("head %d names %zu classes and has %d", index, labels_.size(), classes_));
    }

    const char* type() const override { return "byte_map"; }

    void run(const HeadInput& in, HeadOutput& out) const override {
        // One label per document byte: the positions of a query prefix get none.
        out.byte_labels.assign(static_cast<size_t>(in.T - in.first_position), 0);
        std::vector<float> logits(static_cast<size_t>(classes_));
        for (int t = in.first_position; t < in.T; ++t) {
            const float* h = in.hidden + static_cast<size_t>(t) * d_;
            int best = 0;
            for (int c = 0; c < classes_; ++c) {
                float a = bias_[c];
                const float* w = weight_ + static_cast<size_t>(c) * d_;
                for (int i = 0; i < d_; ++i) a += w[i] * h[i];
                logits[c] = a;
                if (a > logits[best]) best = c;
            }
            out.byte_labels[static_cast<size_t>(t - in.first_position)] = static_cast<uint8_t>(best);
        }
        out.computed = true;
    }

    std::string describe() const override {
        json::Writer w = describe_begin();
        w.field("classes", classes_).end_object();
        return w.take();
    }

private:
    int d_ = 0, classes_ = 0;
    const float* weight_ = nullptr;
    const float* bias_ = nullptr;
};

}  // namespace

std::unique_ptr<Head> load_byte_map_head(LoadContext& ctx, int index) {
    return std::make_unique<ByteMapHead>(ctx, index);
}

}  // namespace pb
