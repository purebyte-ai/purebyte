#include "runtime/forward.h"

#include <cstring>

namespace pb {

void prepare_workspace(const Model& m, TeamWorkspace& ws, int T, int members) {
    const size_t rows = static_cast<size_t>(T) * m.d_model;
    if (ws.residual.size() < rows) ws.residual.resize(rows);
    if (ws.hidden.size() < rows) ws.hidden.resize(rows);
    if (m.lookahead && ws.look.size() < rows) ws.look.resize(rows);
    if (ws.inv.size() < static_cast<size_t>(T)) ws.inv.resize(static_cast<size_t>(T));
    const size_t shared = m.scratch_floats(T);
    if (ws.shared.size() < shared) ws.shared.resize(shared);
    if (static_cast<int>(ws.member.size()) < members) ws.member.resize(static_cast<size_t>(members));
    const size_t own = m.member_scratch_floats(T);
    for (int i = 0; i < members; ++i)
        if (ws.member[i].size() < own) ws.member[i].resize(own);
    if (m.exit_head) {
        if (ws.exit_features.size() < m.exit_head->feature_floats())
            ws.exit_features.resize(m.exit_head->feature_floats());
        if (ws.exit_sums.size() < m.exit_head->sum_doubles()) ws.exit_sums.resize(m.exit_head->sum_doubles());
    }
}

const float* forward_window(const Model& m, const kernels::Kernels& k, const Team& team, TeamWorkspace& ws,
                            const uint8_t* bytes, int T, int history, bool early_exit, StreamCarry* carry) {
    const int d = m.d_model;
    float* X = ws.residual.data();
    float* own = ws.member[team.member].data();
    const BlockContext ctx{k, team, ws.shared.data(), own, carry ? carry->position : 0};

    {
        const auto [t0, t1] = team.share(T);
        for (int t = t0; t < t1; ++t)
            std::memcpy(X + static_cast<size_t>(t) * d, m.embedding + static_cast<size_t>(bytes[t]) * d,
                        sizeof(float) * d);
        if (m.ngram && m.ngram->layer() == 0) m.ngram->apply(k, bytes, history, t0, t1, X, own);
    }
    team.sync();
    size_t state_offset = 0;
    for (size_t l = 0; l < m.blocks.size(); ++l) {
        if (m.ngram && l > 0 && m.ngram->layer() == static_cast<int>(l)) {
            const auto [t0, t1] = team.share(T);
            m.ngram->apply(k, bytes, history, t0, t1, X, own);
            team.sync();
        }
        const Block& block = *m.blocks[l];
        float* state = carry ? carry->state.data() + state_offset : nullptr;
        state_offset += block.stream_state_floats();
        block.forward(ctx, X, T, state);  // ends with a team barrier
        if (early_exit && m.exit_head && m.exit_head->layer() == static_cast<int>(l) + 1) {
            // Only the leader scores; whatever happens, it must reach the barrier, or the members would wait there
            // forever. A failure is published in the workspace and the window stops for every member.
            if (team.leader()) {
                try {
                    ws.exited = m.exit_head->score(X, T, ws.exit_features.data(), ws.exit_sums.data()) <
                                static_cast<double>(m.exit_head->threshold());
                } catch (...) {
                    ws.failure = std::current_exception();
                    ws.exited = true;
                }
            }
            team.sync();
            if (ws.exited) return nullptr;
        }
    }
    float* H = ws.hidden.data();
    {
        const auto [t0, t1] = team.share(T, 16);
        float* inv = ws.inv.data();
        k.rms_inv(X + static_cast<size_t>(t0) * d, d, t1 - t0, d, inv + t0);
        for (int t = t0; t < t1; ++t)
            k.scale_mul(X + static_cast<size_t>(t) * d, inv[t], m.final_norm, H + static_cast<size_t>(t) * d, d);
    }
    team.sync();
    if (!m.lookahead) return H;
    {
        const auto [t0, t1] = team.share(T);
        m.lookahead->apply(H, T, t0, t1, ws.look.data());
    }
    team.sync();
    return ws.look.data();
}

bool run_heads(const Model& m, const kernels::Kernels& k, const float* hidden, int T, int first_position,
               const SpanBias& bias, bool ungated, std::vector<HeadOutput>& outputs) {
    outputs.assign(m.heads.size(), HeadOutput());
    const HeadInput in{k, hidden, T, first_position, bias};
    for (const auto& h : m.heads)  // first every head no gate controls (the gates are among them)
        if (h->gated_by() < 0 && h.get() != m.exit_head) h->run(in, outputs[h->index()]);
    bool gated = false;
    for (const auto& h : m.heads) {
        if (h->gated_by() < 0) continue;
        if (ungated || m.heads[h->gated_by()]->positive(outputs[h->gated_by()]))
            h->run(in, outputs[h->index()]);
        else
            gated = true;
    }
    return gated;
}

uint64_t window_digest(const float* hidden, size_t count, const std::vector<HeadOutput>& outputs) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* data, size_t bytes) {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < bytes; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    };
    mix(hidden, count * sizeof(float));
    for (const HeadOutput& o : outputs)
        if (o.computed) mix(o.values.data(), o.values.size() * sizeof(float));
    return h;
}

}  // namespace pb
