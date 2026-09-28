// The body of a model over one window (or one chunk of a stream), run by a team of threads, and the heads after it.
#pragma once

#include <cstdint>
#include <vector>

#include "core/threads.h"
#include "heads/head.h"
#include "model/model.h"
#include "runtime/session.h"

namespace pb {

// What a stream carries from one chunk to the next.
struct StreamCarry {
    int64_t position = 0;       // bytes already processed
    std::vector<float> state;   // the blocks' carried state, concatenated in block order
    std::vector<uint8_t> tail;  // the last bytes seen (as many as the n-gram tables look back)
};

// Sizes the workspace for T positions and `members` threads. Called by the leader before the members start.
void prepare_workspace(const Model& model, TeamWorkspace& ws, int T, int members);

// Runs embedding, n-grams and blocks over bytes[0..T) and returns the final hidden states [T, d_model] (inside `ws`),
// or nullptr when early exit stopped the window, or when the part only the leader runs failed: `ws.failure` then
// holds the exception, which the caller (the leader) rethrows after the members have left. Every member of `team`
// calls it (SPMD); nothing in it allocates, and no member leaves it by an exception while the others wait at a
// barrier. `bytes[-history .. -1]` are the bytes before a stream chunk; `carry` is null for a window.
const float* forward_window(const Model& model, const kernels::Kernels& kernels, const Team& team, TeamWorkspace& ws,
                            const uint8_t* bytes, int T, int history, bool early_exit, StreamCarry* carry);

// Runs the heads on hidden states [T, d_model] (leader only). Gated heads run only when their gate is positive,
// unless `ungated`. `outputs` gets one entry per head (the exit head's is never computed); returns true when some
// gate said negative.
bool run_heads(const Model& model, const kernels::Kernels& kernels, const float* hidden, int T, int first_position,
               const SpanBias& bias, bool ungated, std::vector<HeadOutput>& outputs);

// FNV-1a of the hidden states' bytes followed by the values of every computed head, in head order: a fingerprint of
// the numerics of one window, for reproducibility checks.
uint64_t window_digest(const float* hidden, size_t count, const std::vector<HeadOutput>& outputs);

}  // namespace pb
