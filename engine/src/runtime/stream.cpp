// The stream context: every input is one sequence, run chunk after chunk with the blocks' state carried (scan.h).
// Inputs are independent, so teams take whole inputs; the members of a team split each chunk as for a window.
#include <algorithm>
#include <atomic>
#include <mutex>

#include "core/failure.h"
#include "runtime/forward.h"
#include "runtime/scan.h"

namespace pb {

namespace {

// What the leader of a team publishes to its members before each chunk.
struct Job {
    bool active = false;  // false: no input left, the members stop
    size_t input = 0;
    size_t chunk = 0;  // index within the input
};

void check_options(const ScanOptions& o) {
    if (o.stride > 0 && o.stride != o.window)
        fail(PB_ERR_ARGUMENT, "this model reads its input as one stream of consecutive chunks: leave the stride unset");
    if (o.prefilter)
        fail(PB_ERR_ARGUMENT, "the prefilter cannot skip parts of a stream: this model has the stream context");
    if (o.early_exit) fail(PB_ERR_UNSUPPORTED, "early exit needs a model with the window context");
    if (!o.queries.empty()) fail(PB_ERR_ARGUMENT, "this model takes no queries");
}

}  // namespace

ScanResult scan_streams(Session& session, const Model& m, const std::vector<Bytes>& inputs, const ScanOptions& o) {
    check_options(o);
    ScanResult result;
    result.window = o.window > 0 ? o.window : m.window.size;
    result.stride = result.window;
    result.min_length = o.min_length > 0 ? o.min_length : m.window.min_length;

    // The chunks of every input, and the inputs that have any.
    std::vector<size_t> jobs;
    result.inputs.resize(inputs.size());
    for (size_t i = 0; i < inputs.size(); ++i) {
        const int64_t size = static_cast<int64_t>(inputs[i].size);
        if (size == 0 || size < result.min_length) continue;
        if (o.whole && size > (int64_t(1) << 30))
            fail(PB_ERR_ARGUMENT, "an input larger than 1 GiB cannot be one chunk");
        const int64_t chunk = o.whole ? size : result.window;
        for (int64_t start = 0; start < size; start += chunk) {
            WindowResult w;
            w.start = start;
            w.length = static_cast<int32_t>(std::min<int64_t>(chunk, size - start));
            result.inputs[i].push_back(std::move(w));
        }
        jobs.push_back(i);
    }
    if (jobs.empty()) return result;

    const int team_size = session.team_size(jobs.size());
    const int teams = std::max(1, session.threads() / team_size);
    const int look_back = m.ngram ? m.ngram->max_order() - 1 : 0;  // bytes before a chunk that the n-grams read
    const kernels::Kernels& k = session.kernels();
    std::vector<TeamWorkspace*> workspaces;
    std::vector<Barrier*> barriers;
    std::vector<Job> published(static_cast<size_t>(teams));
    std::vector<StreamCarry> carries(static_cast<size_t>(teams));
    for (int t = 0; t < teams; ++t) {
        workspaces.push_back(&session.workspace(t));
        barriers.push_back(&session.barrier(t, team_size));
    }
    std::atomic<size_t> next{0};
    std::atomic<bool> stop{false};
    std::mutex error_mutex;
    std::exception_ptr error;
    auto record_error = [&]() {
        std::lock_guard<std::mutex> lock(error_mutex);
        if (!error) error = std::current_exception();
        stop = true;
    };

    session.pool().run([&](int thread) {
        const int team_id = thread / team_size;
        if (team_id >= teams) return;
        const Team team{thread % team_size, team_size, barriers[team_id]};
        TeamWorkspace& ws = *workspaces[team_id];
        Job& job = published[team_id];
        StreamCarry& carry = carries[team_id];
        for (;;) {
            if (team.leader()) {
                try {
                    // The next chunk of the current input, else the first chunk of the next input (fresh state).
                    if (job.active && job.chunk + 1 < result.inputs[job.input].size()) {
                        ++job.chunk;
                    } else {
                        const size_t idx = stop ? jobs.size() : next.fetch_add(1);
                        job.active = idx < jobs.size();
                        if (job.active) {
                            job.input = jobs[idx];
                            job.chunk = 0;
                            carry.position = 0;
                            carry.state.assign(m.stream_state_floats(), 0.f);
                        }
                    }
                    if (stop) job.active = false;
                    if (job.active) prepare_workspace(m, ws, result.inputs[job.input][job.chunk].length, team.size);
                } catch (...) {
                    record_error();
                    job.active = false;
                }
            }
            team.sync();
            if (!job.active) break;
            WindowResult& w = result.inputs[job.input][job.chunk];
            const int T = w.length;
            const int history = static_cast<int>(std::min<int64_t>(w.start, look_back));
            const uint8_t* bytes = inputs[job.input].data + w.start;
            const float* hidden = forward_window(m, k, team, ws, bytes, T, history, false, &carry);
            if (!team.leader()) continue;
            try {
                finish_window(m, k, hidden, T, 0, w.start, o, w);
                carry.position += T;
            } catch (...) {
                record_error();
            }
        }
    });
    if (error) std::rethrow_exception(error);
    return result;
}

}  // namespace pb
