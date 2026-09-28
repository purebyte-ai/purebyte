// A session: the kernels in use, a pool of persistent worker threads, and the scratch memory of every team. It owns
// no model; one session can run any number of models, one call at a time.
#pragma once

#include <exception>
#include <memory>
#include <string>
#include <vector>

#include "core/threads.h"
#include "kernels/kernels.h"

namespace pb {

// Buffers of one team, grown on demand and reused from window to window.
struct TeamWorkspace {
    std::vector<float> residual;             // X [T, d_model]
    std::vector<float> hidden;               // final hidden states [T, d_model]
    std::vector<float> look;                 // lookahead output [T, d_model]
    std::vector<float> inv;                  // [T]: inverse RMS of the final norm
    std::vector<float> shared;               // the blocks' shared scratch
    std::vector<std::vector<float>> member;  // each member's private scratch
    std::vector<uint8_t> input;              // window bytes built by the runtime (query prefix + document)
    std::vector<float> exit_features;        // the exit head's scratch (it runs between barriers: no allocation)
    std::vector<double> exit_sums;
    // Published by the leader to the members (read after a barrier).
    size_t task = 0;
    bool exited = false;
    std::exception_ptr failure;  // what the leader's part of a window threw (the members return with it)
};

class Session {
public:
    // Throws Failure(PB_ERR_ARGUMENT) for bad counts and PB_ERR_UNSUPPORTED for a kernel this CPU cannot run.
    Session(int threads, int intra_threads, const std::string& kernel);

    const kernels::Kernels& kernels() const { return *kernels_; }
    int threads() const { return pool_.size(); }
    int intra_threads() const { return intra_; }
    ThreadPool& pool() { return pool_; }

    // Threads per team for a batch of `windows` windows: `intra_threads` when set, else the idle threads of a batch
    // smaller than the pool join its windows, up to 8 per window.
    int team_size(size_t windows) const;
    // The workspace and barrier of team `team` (created on first use).
    TeamWorkspace& workspace(int team);
    Barrier& barrier(int team, int members);

private:
    const kernels::Kernels* kernels_;
    int intra_;
    ThreadPool pool_;
    std::vector<std::unique_ptr<TeamWorkspace>> workspaces_;
    std::vector<std::unique_ptr<Barrier>> barriers_;
    std::vector<int> barrier_sizes_;
};

}  // namespace pb
