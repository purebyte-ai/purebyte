// Block type `ssm_v2`: a Mamba-2 style selective state-space block, with ternary or float projections
// (model/projection.h).
//
//   xn = RMSNorm(x)
//   [z | xBC] = in_proj(xn)                    z: d_inner, xBC = [x: d_inner | B: G*N | C: G*N]
//   dt = softplus(dt_bias + dt_proj(xn))       one step per head; dA = exp(-exp(A_log) * dt)
//   xBC = SiLU(causal depthwise conv(xBC))     K taps (none when conv_width = 0)
//   y = selective scan(x, B, C, dt, dA) + D * x
//   y = GatedRMSNorm(y * SiLU(z))              RMSNorm per group of d_inner / G channels
//   x = x + out_proj(y)
//
// Directions: `causal` scans left to right; `bimamba` adds a scan over the reversed sequence (both with D);
// `hydra` combines shifted scans of both directions and adds D * x once: y[t] = yf[t-1] + yb[t+1] + D * x[t].
// Only a causal block can stream: its whole memory is the scan state and the last K-1 convolution inputs.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "blocks/block.h"
#include "core/failure.h"
#include "core/json.h"
#include "model/projection.h"

namespace pb {

namespace {

enum class Direction { Causal, BiMamba, Hydra };

const char* direction_name(Direction d) {
    return d == Direction::Causal ? "causal" : d == Direction::BiMamba ? "bimamba" : "hydra";
}

inline float softplus(float z) { return z > 20.f ? z : std::log1p(std::exp(z)); }

class SsmBlock final : public Block {
public:
    SsmBlock(LoadContext& ctx, int index);

    const char* type() const override { return "ssm_v2"; }
    size_t scratch_floats(int T) const override { return layout(T).total; }
    size_t member_scratch_floats(int) const override { return fold_floats() + static_cast<size_t>(H_); }
    size_t position_floats() const override { return static_cast<size_t>(proj_out_); }
    size_t stream_state_floats() const override {
        return direction_ == Direction::Causal ? history_floats() + scan_state_floats() : 0;
    }
    void forward(const BlockContext& ctx, float* X, int T, float* carried) const override;
    std::string describe() const override;

private:
    struct Layout {  // offsets in the shared scratch, in floats
        size_t xn, inv, proj, dt, da, xbc, y, out, state, xrev, dtr, dar, xbcb, yf, yb, total;
    };
    Layout layout(int T) const;
    int history_rows() const { return K_ > 0 ? K_ - 1 : 0; }
    size_t history_floats() const { return static_cast<size_t>(history_rows()) * cw_; }
    size_t scan_state_floats() const { return static_cast<size_t>(H_) * blocks_per_head_ * N_ * 8; }
    size_t fold_floats() const { return 8 * static_cast<size_t>(std::max(d_, di_)); }

    void conv(const BlockContext& ctx, const float* src, size_t lds, int history, float* dst, int T) const;
    void scan(const BlockContext& ctx, const float* xbc, const float* dt, const float* da, float* y, float* states,
              bool include_D, bool zero_state, int T) const;

    int d_ = 0, H_ = 0, P_ = 0, N_ = 0, G_ = 0, K_ = 0, di_ = 0, cw_ = 0, proj_out_ = 0, blocks_per_head_ = 0;
    Direction direction_ = Direction::Causal;
    const float* norm_ = nullptr;
    const float* gated_norm_ = nullptr;
    const float* D_ = nullptr;
    const float* dt_bias_ = nullptr;
    const float* dt_proj_ = nullptr;
    const float* conv_bias_ = nullptr;
    std::vector<float> neg_A_;    // -exp(A_log), per head
    std::vector<float> conv_kT_;  // convolution weights transposed to [K][channels]
    Projection in_proj_, out_proj_;
};

SsmBlock::SsmBlock(LoadContext& ctx, int index) {
    const gguf::File& f = ctx.file();
    const std::string p = "blocks." + std::to_string(index) + ".";
    // A per-block value (purebyte.block.<i>.<name>) overrides the model-wide one (purebyte.<name>).
    auto hyper = [&](const char* name, int64_t lo, int64_t hi) {
        const std::string own = LoadContext::key("block", index, name);
        return static_cast<int>(checked_int(f, f.has(own) ? own : std::string("purebyte.") + name, lo, hi));
    };
    d_ = ctx.d_model();
    H_ = hyper("n_heads", 1, 4096);
    P_ = hyper("d_head", 1, 4096);
    N_ = hyper("d_state", 1, 4096);
    G_ = hyper("n_groups", 1, 4096);
    K_ = hyper("conv_width", 0, 64);
    if (H_ % G_ != 0) fail(PB_ERR_FORMAT, format("block %d: %d heads are not a multiple of %d groups", index, H_, G_));
    di_ = H_ * P_;
    cw_ = di_ + 2 * G_ * N_;
    proj_out_ = 2 * di_ + 2 * G_ * N_;
    blocks_per_head_ = (P_ + 7) / 8;
    // What one position costs, whatever the window (spec/FORMAT.md, section 13): the in_proj row every position
    // writes, and the scan's state (n_heads x d_head x d_state floats, updated at every position).
    constexpr int kMaxRow = 1 << 20;
    constexpr int64_t kMaxScan = int64_t(1) << 20;
    if (proj_out_ > kMaxRow || static_cast<int64_t>(H_) * P_ * N_ > kMaxScan)
        fail(PB_ERR_FORMAT, format("block %d: in_proj rows of %d floats and a scan of %lld floats per position; this "
                                   "runtime reads at most %d and %lld",
                                   index, proj_out_, static_cast<long long>(static_cast<int64_t>(H_) * P_ * N_),
                                   kMaxRow, static_cast<long long>(kMaxScan)));

    const std::string own_direction = LoadContext::key("block", index, "direction");
    const std::string dir = f.text(f.has(own_direction) ? own_direction : "purebyte.direction", "causal");
    if (dir == "causal")
        direction_ = Direction::Causal;
    else if (dir == "bimamba")
        direction_ = Direction::BiMamba;
    else if (dir == "hydra")
        direction_ = Direction::Hydra;
    else
        fail(PB_ERR_UNSUPPORTED,
             format("block %d: direction `%s` (known: causal, bimamba, hydra)", index, dir.c_str()));

    norm_ = ctx.f32(p + "norm.weight", {d_});
    gated_norm_ = ctx.f32(p + "gated_norm.weight", {di_});
    const float* A_log = ctx.f32(p + "A_log", {H_});
    D_ = ctx.f32(p + "D", {H_});
    dt_bias_ = ctx.f32(p + "dt_bias", {H_});
    dt_proj_ = ctx.f32(p + "dt_proj.weight", {H_, d_});
    neg_A_.resize(static_cast<size_t>(H_));
    for (int h = 0; h < H_; ++h) neg_A_[h] = -std::exp(A_log[h]);
    if (K_ > 0) {
        const float* w = ctx.f32(p + "conv1d.weight", {cw_, 1, K_});
        conv_bias_ = ctx.f32(p + "conv1d.bias", {cw_});
        conv_kT_.resize(static_cast<size_t>(K_) * cw_);
        for (int c = 0; c < cw_; ++c)
            for (int k = 0; k < K_; ++k)
                conv_kT_[static_cast<size_t>(k) * cw_ + c] = w[static_cast<size_t>(c) * K_ + k];
    }
    const int group = static_cast<int>(checked_int(f, "purebyte.tern.group", 1, 1 << 24, 0));  // 0: not declared
    in_proj_ = Projection::load(ctx, p + "in_proj.weight", proj_out_, d_, group);
    out_proj_ = Projection::load(ctx, p + "out_proj.weight", d_, di_, group);
}

SsmBlock::Layout SsmBlock::layout(int T) const {
    const size_t t = static_cast<size_t>(T);
    Layout l{};
    size_t at = 0;
    auto take = [&](size_t n) {
        const size_t offset = at;
        at += n;
        return offset;
    };
    l.xn = take(t * d_);
    l.inv = take(t);
    l.proj = take((t + history_rows()) * proj_out_);  // leading rows: the carried convolution inputs when streaming
    l.dt = take(t * H_);
    l.da = take(t * H_);
    l.xbc = take(t * cw_);
    l.y = take(t * di_);
    l.out = take(t * d_);
    l.state = take(scan_state_floats());
    if (direction_ != Direction::Causal) {
        l.xrev = take(t * cw_);
        l.dtr = take(t * H_);
        l.dar = take(t * H_);
        l.xbcb = take(t * cw_);
        l.yf = take(t * di_);
        l.yb = take(t * di_);
    }
    l.total = at;
    return l;
}

void SsmBlock::conv(const BlockContext& ctx, const float* src, size_t lds, int history, float* dst, int T) const {
    const auto [t0, t1] = ctx.team.share(T);
    if (K_ > 0) {
        ctx.kernels.conv_silu(src, lds, history, dst, cw_, conv_kT_.data(), conv_bias_, t0, t1, cw_, K_);
    } else {
        for (int t = t0; t < t1; ++t) ctx.kernels.silu(src + t * lds, dst + static_cast<size_t>(t) * cw_, cw_);
    }
}

// Scans every (head, block of 8 channels) item; items are independent, so the team splits them.
void SsmBlock::scan(const BlockContext& ctx, const float* xbc, const float* dt, const float* da, float* y,
                    float* states, bool include_D, bool zero_state, int T) const {
    const int items = H_ * blocks_per_head_, heads_per_group = H_ / G_, GN = G_ * N_;
    const auto [i0, i1] = ctx.team.share(items);
    for (int item = i0; item < i1; ++item) {
        const int h = item / blocks_per_head_, block = item % blocks_per_head_;
        const int g = h / heads_per_group, c0 = h * P_ + block * 8;
        kernels::ScanBlock b;
        b.state = states + static_cast<size_t>(item) * N_ * 8;
        if (zero_state) std::memset(b.state, 0, sizeof(float) * N_ * 8);
        b.x = xbc + c0;
        b.x_stride = cw_;
        b.B = xbc + di_ + g * N_;
        b.C = xbc + di_ + GN + g * N_;
        b.bc_stride = cw_;
        b.dA = da + h;
        b.dt = dt + h;
        b.t_stride = H_;
        b.y = y + c0;
        b.y_stride = di_;
        b.D = D_[h];
        b.include_D = include_D;
        b.T = T;
        b.channels = std::min(8, P_ - block * 8);
        b.N = N_;
        ctx.kernels.scan(b);
    }
}

void SsmBlock::forward(const BlockContext& ctx, float* X, int T, float* carried) const {
    const kernels::Kernels& k = ctx.kernels;
    const Team& team = ctx.team;
    const Layout l = layout(T);
    float* s = ctx.scratch;
    const size_t hist = static_cast<size_t>(history_rows());
    float* Xn = s + l.xn;
    float* inv = s + l.inv;
    float* Proj = s + l.proj + hist * proj_out_;  // row 0; rows -hist .. -1 hold carried convolution inputs
    float* DT = s + l.dt;
    float* DA = s + l.da;
    float* xbc = s + l.xbc;
    float* Y = s + l.y;
    float* Out = s + l.out;
    float* fold = ctx.member_scratch;
    float* dt_dots = ctx.member_scratch + fold_floats();
    const bool streaming = carried != nullptr;

    // 1. RMSNorm of the residual stream.
    {
        const auto [t0, t1] = team.share(T, 16);
        k.rms_inv(X + static_cast<size_t>(t0) * d_, d_, t1 - t0, d_, inv + t0);
        for (int t = t0; t < t1; ++t)
            k.scale_mul(X + static_cast<size_t>(t) * d_, inv[t], norm_, Xn + static_cast<size_t>(t) * d_, d_);
    }
    team.sync();
    // 2. in_proj: every position, output rows split across the team.
    {
        const auto [o0, o1] = team.share(proj_out_, 8);
        k.gemm(in_proj_.matrix(), Xn, d_, T, Proj, proj_out_, o0, o1, fold);
    }
    // 3. Step size and decay per (position, head).
    {
        const auto [t0, t1] = team.share(T);
        for (int t = t0; t < t1; ++t) {
            k.dot_rows(dt_proj_, d_, H_, Xn + static_cast<size_t>(t) * d_, d_, dt_dots);
            for (int h = 0; h < H_; ++h) {
                const float dt = softplus(dt_bias_[h] + dt_dots[h]);
                DT[static_cast<size_t>(t) * H_ + h] = dt;
                DA[static_cast<size_t>(t) * H_ + h] = std::exp(neg_A_[h] * dt);
            }
        }
    }
    // Streaming: the convolution sees the inputs of the previous chunk (none at the start of the stream).
    int history = 0;
    if (streaming && hist > 0) {
        history = static_cast<int>(std::min<int64_t>(static_cast<int64_t>(hist), ctx.position));
        if (team.leader())
            for (size_t r = 0; r < hist; ++r)
                std::memcpy(Proj - (hist - r) * proj_out_ + di_, carried + r * cw_, sizeof(float) * cw_);
    }
    team.sync();

    // 4. Convolution + SiLU, then the scan, per direction.
    if (direction_ == Direction::Causal) {
        conv(ctx, Proj + di_, proj_out_, history, xbc, T);
        team.sync();
        float* states = streaming ? carried + history_floats() : s + l.state;
        scan(ctx, xbc, DT, DA, Y, states, true, !streaming, T);
    } else {
        conv(ctx, Proj + di_, proj_out_, 0, xbc, T);
        {
            // The reversed sequence: pre-convolution rows and step sizes in reverse order.
            const auto [t0, t1] = team.share(T);
            for (int t = t0; t < t1; ++t) {
                const size_t src = static_cast<size_t>(T - 1 - t);
                std::memcpy(s + l.xrev + static_cast<size_t>(t) * cw_, Proj + src * proj_out_ + di_,
                            sizeof(float) * cw_);
                std::memcpy(s + l.dtr + static_cast<size_t>(t) * H_, DT + src * H_, sizeof(float) * H_);
                std::memcpy(s + l.dar + static_cast<size_t>(t) * H_, DA + src * H_, sizeof(float) * H_);
            }
        }
        team.sync();
        conv(ctx, s + l.xrev, cw_, 0, s + l.xbcb, T);
        const bool hydra = direction_ == Direction::Hydra;
        scan(ctx, xbc, DT, DA, s + l.yf, s + l.state, !hydra, true, T);
        team.sync();
        scan(ctx, s + l.xbcb, s + l.dtr, s + l.dar, s + l.yb, s + l.state, !hydra, true, T);
        team.sync();
        const float* Yf = s + l.yf;
        const float* Yb = s + l.yb;
        const auto [t0, t1] = team.share(T);
        for (int t = t0; t < t1; ++t) {
            float* y = Y + static_cast<size_t>(t) * di_;
            if (hydra) {
                // shift(Yf)[t] + flip(shift(Yb))[t]: Yb is in reverse order, so the entry for t is row T-2-t.
                const int back = T - 2 - t;
                for (int i = 0; i < di_; ++i) {
                    const float f = t > 0 ? Yf[static_cast<size_t>(t - 1) * di_ + i] : 0.f;
                    const float b = back >= 0 ? Yb[static_cast<size_t>(back) * di_ + i] : 0.f;
                    y[i] = f + b;
                }
                const float* x = xbc + static_cast<size_t>(t) * cw_;  // the x part of the forward direction
                for (int h = 0; h < H_; ++h)
                    for (int c = h * P_; c < (h + 1) * P_; ++c) y[c] = y[c] + D_[h] * x[c];
            } else {
                for (int i = 0; i < di_; ++i)
                    y[i] = Yf[static_cast<size_t>(t) * di_ + i] + Yb[static_cast<size_t>(T - 1 - t) * di_ + i];
            }
        }
    }
    team.sync();

    // 5. Gate with SiLU(z), then RMSNorm per group of channels.
    {
        const auto [t0, t1] = team.share(T, 16);
        for (int t = t0; t < t1; ++t)
            k.mul_silu(Y + static_cast<size_t>(t) * di_, Proj + static_cast<size_t>(t) * proj_out_, di_);
        const int gs = di_ / G_;
        for (int g = 0; g < G_; ++g) {
            k.rms_inv(Y + static_cast<size_t>(t0) * di_ + g * gs, di_, t1 - t0, gs, inv + t0);
            for (int t = t0; t < t1; ++t) {
                float* row = Y + static_cast<size_t>(t) * di_ + g * gs;
                k.scale_mul(row, inv[t], gated_norm_ + g * gs, row, gs);
            }
        }
    }
    team.sync();
    // 6. out_proj.
    {
        const auto [o0, o1] = team.share(d_, 8);
        k.gemm(out_proj_.matrix(), Y, di_, T, Out, d_, o0, o1, fold);
    }
    team.sync();
    // 7. Residual; when streaming, keep the last K-1 convolution inputs (rows -hist .. T-1 are contiguous).
    {
        const auto [t0, t1] = team.share(T);
        for (size_t i = static_cast<size_t>(t0) * d_; i < static_cast<size_t>(t1) * d_; ++i) X[i] += Out[i];
        if (streaming && hist > 0 && team.leader())
            for (size_t r = 0; r < hist; ++r)
                std::memcpy(carried + r * cw_,
                            Proj + (static_cast<ptrdiff_t>(T) - static_cast<ptrdiff_t>(hist - r)) * proj_out_ + di_,
                            sizeof(float) * cw_);
    }
    team.sync();
}

std::string SsmBlock::describe() const {
    json::Writer w;
    w.begin_object()
        .field("type", "ssm_v2")
        .field("direction", direction_name(direction_))
        .field("n_heads", H_)
        .field("d_head", P_)
        .field("d_state", N_)
        .field("n_groups", G_)
        .field("conv_width", K_)
        .field("projections", in_proj_.ternary() ? "ternary" : "float")
        .end_object();
    return w.take();
}

}  // namespace

std::unique_ptr<Block> load_ssm_block(LoadContext& ctx, int index) { return std::make_unique<SsmBlock>(ctx, index); }

}  // namespace pb
