// A block of the body: transforms the residual stream X [T, d_model] of one window in place.
//
// Blocks are listed in the file (`purebyte.block.<i>.type`) and built from the registry in blocks/registry.cpp. To add
// a block type: implement this interface in a new file and add one line to that registry. The forward is SPMD: every
// member of a team calls `forward` with the same arguments, takes its share of each stage (Team::share) and meets the
// others with Team::sync() before a stage reads what another member wrote.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "core/threads.h"
#include "kernels/kernels.h"
#include "model/load_context.h"

namespace pb {

struct BlockContext {
    const kernels::Kernels& kernels;
    const Team& team;
    float* scratch;         // shared by the team: scratch_floats(T) floats
    float* member_scratch;  // private to this member: member_scratch_floats() floats
    int64_t position;       // streaming: position of row 0 in the stream; 0 for a window
};

class Block {
public:
    virtual ~Block() = default;
    virtual const char* type() const = 0;

    // Floats of scratch forward() needs for T positions: shared by the team, and private to each member.
    virtual size_t scratch_floats(int T) const = 0;
    virtual size_t member_scratch_floats(int T) const = 0;
    // Floats of the widest row forward() writes per position (ssm_v2: its in_proj output; attention: q, k and v). The
    // loader bounds the declared window size times it (spec/FORMAT.md, section 13).
    virtual size_t position_floats() const = 0;

    // Streaming (the input processed as ONE sequence in chunks): floats of state carried from one chunk to the next.
    // 0 means the block cannot stream (it needs the whole window, e.g. attention or a bidirectional scan).
    virtual size_t stream_state_floats() const { return 0; }

    // X [T, d_model] in place. `state` is null for a window; when streaming it holds the carried state (all zeros at
    // the start of a stream) and is updated for the next chunk. It runs SPMD between the barriers of a team, so it
    // must not allocate or throw (a member that left by an exception would leave the others waiting at a barrier):
    // everything it needs comes from the scratch sized by scratch_floats and member_scratch_floats.
    virtual void forward(const BlockContext& ctx, float* X, int T, float* state) const = 0;

    // JSON object describing the block (for pb_model_describe).
    virtual std::string describe() const = 0;
};

using BlockLoader = std::unique_ptr<Block> (*)(LoadContext& ctx, int index);

struct BlockType {
    const char* name;
    BlockLoader load;
};

// The block type called `name`, or nullptr when this library does not implement it.
const BlockType* find_block_type(const std::string& name);
// Comma-separated names of the known block types, for error messages.
std::string known_block_types();

// Loaders of the built-in block types (one per module).
std::unique_ptr<Block> load_ssm_block(LoadContext& ctx, int index);
std::unique_ptr<Block> load_attention_block(LoadContext& ctx, int index);

}  // namespace pb
