#include "runtime/encode.h"

#include "core/failure.h"
#include "runtime/forward.h"

namespace pb {

std::vector<float> encode(Session& session, const Model& m, Bytes input) {
    if (input.size == 0) fail(PB_ERR_ARGUMENT, "nothing to encode: the input is empty");
    if (input.size > (1u << 30)) fail(PB_ERR_ARGUMENT, "input larger than 1 GiB: use windows");
    const int T = static_cast<int>(input.size);
    const int members = session.threads();
    TeamWorkspace& ws = session.workspace(0);
    Barrier& barrier = session.barrier(0, members);
    prepare_workspace(m, ws, T, members);
    std::vector<float> out;
    session.pool().run([&](int thread) {
        const Team team{thread, members, &barrier};
        const float* hidden = forward_window(m, session.kernels(), team, ws, input.data, T, 0, false, nullptr);
        if (team.leader()) out.assign(hidden, hidden + static_cast<size_t>(T) * m.d_model);
    });
    return out;
}

}  // namespace pb
