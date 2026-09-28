// A linear map stored ternary (2-bit codes + one F32 scale per group of inputs) or in F32.
//
// Ternary layout in the file (`<name>.packed` + `<name>.scales`): four codes per byte, code j of a byte in bits
// 2*(j % 4); 00 = 0, 01 = +1, 10 = -1, 11 is invalid. Codes are unpacked once at load time to one int8 per weight,
// which the kernels fold to floats (code * scale, exact) a few rows at a time.
#pragma once

#include <string>
#include <vector>

#include "kernels/kernels.h"
#include "model/load_context.h"

namespace pb {

class Projection {
public:
    // Loads `<name>.packed` + `<name>.scales` (ternary, `group` inputs per scale) or `<name>` (F32) of shape [out, in].
    static Projection load(LoadContext& ctx, const std::string& name, int out, int in, int group);

    const kernels::Matrix& matrix() const { return matrix_; }
    bool ternary() const { return matrix_.codes != nullptr; }
    int out() const { return matrix_.out; }
    int in() const { return matrix_.in; }

    Projection() = default;
    Projection(Projection&& other) noexcept { *this = std::move(other); }
    Projection& operator=(Projection&& other) noexcept;
    Projection(const Projection&) = delete;
    Projection& operator=(const Projection&) = delete;

private:
    std::vector<int8_t> codes_;
    kernels::Matrix matrix_;
};

}  // namespace pb
