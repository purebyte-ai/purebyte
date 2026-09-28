#include "runtime/session.h"

#include <algorithm>

#include "core/failure.h"

namespace pb {

namespace {

const kernels::Kernels* pick_kernels(const std::string& name) {
    const kernels::Kernels* k = kernels::find_kernels(name.empty() ? "auto" : name.c_str());
    if (!k) {
        if (name == "scalar" || name == "avx2" || name == "neon")
            fail(PB_ERR_UNSUPPORTED, "the `" + name + "` kernel is not available on this CPU or in this build");
        fail(PB_ERR_ARGUMENT, "unknown kernel `" + name + "` (known: auto, scalar, avx2, neon)");
    }
    return k;
}

int checked_threads(int threads) {
    if (threads < 1 || threads > 1024) fail(PB_ERR_ARGUMENT, format("threads must be within 1..1024, got %d", threads));
    return threads;
}

}  // namespace

Session::Session(int threads, int intra_threads, const std::string& kernel)
    : kernels_(pick_kernels(kernel)), intra_(intra_threads), pool_(checked_threads(threads)) {
    if (intra_threads < 0 || intra_threads > 1024)
        fail(PB_ERR_ARGUMENT, format("intra_threads must be within 0..1024, got %d", intra_threads));
}

int Session::team_size(size_t windows) const {
    const int threads = pool_.size();
    if (intra_ > 0) return std::min(intra_, threads);
    if (windows == 0) return 1;
    // Beyond about 8 threads, the work of one window no longer pays for synchronizing more of them (measured on 12
    // cores: a 64-byte window takes 0.82 ms on 8 threads, 0.92 ms on 12 and 3.1 ms on 23; docs/performance.md).
    constexpr size_t kMaxTeam = 8;
    return static_cast<int>(std::min(kMaxTeam, std::max<size_t>(1, static_cast<size_t>(threads) / windows)));
}

TeamWorkspace& Session::workspace(int team) {
    while (static_cast<int>(workspaces_.size()) <= team) workspaces_.push_back(std::make_unique<TeamWorkspace>());
    return *workspaces_[team];
}

Barrier& Session::barrier(int team, int members) {
    while (static_cast<int>(barriers_.size()) <= team) {
        barriers_.push_back(nullptr);
        barrier_sizes_.push_back(0);
    }
    if (!barriers_[team] || barrier_sizes_[team] != members) {
        barriers_[team] = std::make_unique<Barrier>(members);
        barrier_sizes_[team] = members;
    }
    return *barriers_[team];
}

}  // namespace pb
