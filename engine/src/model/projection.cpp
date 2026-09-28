#include "model/projection.h"

#include "core/failure.h"

namespace pb {

Projection& Projection::operator=(Projection&& other) noexcept {
    codes_ = std::move(other.codes_);
    matrix_ = other.matrix_;
    if (matrix_.codes) matrix_.codes = codes_.data();  // the vector moved; its buffer did not
    other.matrix_ = kernels::Matrix();
    return *this;
}

Projection Projection::load(LoadContext& ctx, const std::string& name, int out, int in, int group) {
    Projection p;
    p.matrix_.out = out;
    p.matrix_.in = in;
    const gguf::Tensor* packed = ctx.tensor_optional(name + ".packed");
    if (!packed) {
        p.matrix_.weights = ctx.f32(name, {out, in});
        return p;
    }
    if (group <= 0 || in % group != 0)
        fail(PB_ERR_FORMAT,
             format("`%s`: %d inputs are not a multiple of the ternary group %d", name.c_str(), in, group));
    const int row_bytes = (in + 3) / 4;
    if (packed->type != gguf::TensorType::I8 || packed->dims.size() != 2 ||
        packed->dims[0] != static_cast<uint64_t>(row_bytes) || packed->dims[1] != static_cast<uint64_t>(out))
        fail(PB_ERR_FORMAT, format("tensor `%s.packed` has shape %s, expected I8 [%d, %d]", name.c_str(),
                                   packed->shape().c_str(), out, row_bytes));
    p.matrix_.group = group;
    p.matrix_.scales = ctx.f32(name + ".scales", {out, in / group});
    p.codes_.resize(static_cast<size_t>(out) * in);
    static const int8_t kValue[4] = {0, 1, -1, 0};
    for (int o = 0; o < out; ++o) {
        const uint8_t* row = packed->data + static_cast<size_t>(o) * row_bytes;
        for (int j = 0; j < row_bytes * 4; ++j) {
            const int code = (row[j / 4] >> (2 * (j % 4))) & 3;
            // 11 is not a ternary value, and the padding of a row must be zero: anything else is a corrupt file.
            if (code == 3 || (j >= in && code != 0))
                fail(PB_ERR_FORMAT,
                     format("tensor `%s.packed` holds an invalid ternary code in row %d", name.c_str(), o));
            if (j < in) p.codes_[static_cast<size_t>(o) * in + j] = kValue[code];
        }
    }
    p.matrix_.codes = p.codes_.data();
    return p;
}

}  // namespace pb
