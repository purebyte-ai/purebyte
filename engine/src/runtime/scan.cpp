#include "runtime/scan.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>

#include "core/failure.h"
#include "runtime/forward.h"
#include "runtime/prefilter.h"
#include "runtime/windows.h"

namespace pb {

namespace {

constexpr size_t kMaxWhole = size_t(1) << 30;  // the longest input run as one window

struct Task {
    size_t input;
    size_t index;  // window index within its input
    int64_t start;
    int32_t length;
};

// Fills the effective window geometry from the options and the model. Windows walk the document by the stride, and
// each holds `span` document bytes (all of it, or what a query region leaves): a stride beyond that would skip bytes,
// and a shortest window beyond it would evaluate nothing. Neither is accepted.
void resolve_geometry(const Model& m, const ScanOptions& o, ScanResult& r) {
    r.window = o.window > 0 ? o.window : m.window.size;
    const int region = m.query.enabled() ? m.query.region : 0;
    if (m.query.enabled() && r.window <= region)
        fail(PB_ERR_ARGUMENT,
             format("a window of %d bytes leaves no room after the %d-byte query region", r.window, m.query.region));
    const int span = r.window - region;
    r.stride = o.stride > 0 ? o.stride : (o.window > 0 ? span - span / 4 : m.window.stride);
    r.min_length = o.min_length > 0 ? o.min_length : m.window.min_length;
    if (r.stride > span)
        fail(PB_ERR_ARGUMENT, region ? format("stride %d is larger than the %d document bytes of a window of %d after "
                                              "the %d-byte query region: bytes between windows would never be seen",
                                              r.stride, span, r.window, region)
                                     : format("stride %d is larger than the window %d: bytes between windows would "
                                              "never be seen",
                                              r.stride, r.window));
    if (!o.whole && r.min_length > span)
        fail(PB_ERR_ARGUMENT, format("windows of %d document bytes are shorter than the model's shortest window (%d "
                                     "bytes): none would be evaluated",
                                     span, r.min_length));
}

}  // namespace

void finish_window(const Model& m, const kernels::Kernels& k, const float* hidden, int T, int first_position,
                   int64_t offset, const ScanOptions& o, WindowResult& w) {
    if (run_heads(m, k, hidden, T, first_position, o.bias, o.ungated, w.heads)) w.flags |= kWindowGated;
    if (o.digest) w.digest = window_digest(hidden, static_cast<size_t>(T) * m.d_model, w.heads);
    for (HeadOutput& h : w.heads)
        for (Span& s : h.spans) {
            s.start += offset;
            s.end += offset;
        }
    if (m.first_choice_head >= 0 && w.heads[m.first_choice_head].computed) {
        const HeadOutput& c = w.heads[m.first_choice_head];
        w.label = c.label;
        w.p_positive = 1.0f - c.values[0];
    }
}

ScanResult scan(Session& session, const Model& m, const std::vector<Bytes>& inputs, const ScanOptions& o) {
    // A per-type operating point must fit the model's tag head before any window runs (a window whose tag head is
    // gated off, or no window at all, must not let a wrong count pass unnoticed).
    if (!o.bias.per_type.empty()) {
        const int tag = m.find_head("tag");
        const size_t entities = tag >= 0 ? m.heads[tag]->labels().size() : 0;
        if (o.bias.per_type.size() != entities)
            fail(PB_ERR_ARGUMENT, format("%zu per-type biases for a model whose tag head has %zu entity types",
                                         o.bias.per_type.size(), entities));
    }
    if (m.streaming) return scan_streams(session, m, inputs, o);
    ScanResult result;
    resolve_geometry(m, o, result);
    const bool query = m.query.enabled();
    std::vector<uint8_t> prefix;
    if (query) {
        if (o.queries.empty()) fail(PB_ERR_ARGUMENT, "this model extracts fields: give at least one query");
        prefix = m.query.encode(o.queries);
    } else if (!o.queries.empty()) {
        fail(PB_ERR_ARGUMENT, "this model takes no queries");
    }
    std::unique_ptr<Prefilter> prefilter;
    if (o.prefilter)
        prefilter = std::make_unique<Prefilter>(o.prefilter_rule.empty() ? Prefilter::kDefaultRule : o.prefilter_rule);
    if (o.early_exit && !m.exit_head) fail(PB_ERR_UNSUPPORTED, "early exit needs a model with an `exit` head");

    // Every window of every input, in order. A query model reads `window - region` document bytes per window.
    const int span = query ? result.window - m.query.region : result.window;
    std::vector<Task> tasks;
    result.inputs.resize(inputs.size());
    for (size_t i = 0; i < inputs.size(); ++i) {
        std::vector<WindowSpan> spans;
        if (o.whole) {
            if (inputs[i].size > kMaxWhole) fail(PB_ERR_ARGUMENT, "an input larger than 1 GiB cannot be one window");
            if (inputs[i].size > 0) spans.push_back({0, static_cast<int32_t>(inputs[i].size)});
        } else {
            spans = enumerate_windows(static_cast<int64_t>(inputs[i].size), span, result.stride, result.min_length);
        }
        result.inputs[i].resize(spans.size());
        for (size_t w = 0; w < spans.size(); ++w) {
            tasks.push_back({i, w, spans[w].start, spans[w].length});
            result.inputs[i][w].start = spans[w].start;
            result.inputs[i][w].length = spans[w].length;
        }
    }
    if (tasks.empty()) return result;

    const int team_size = session.team_size(tasks.size());
    const int teams = std::max(1, session.threads() / team_size);
    std::vector<TeamWorkspace*> workspaces;
    std::vector<Barrier*> barriers;
    for (int t = 0; t < teams; ++t) {
        workspaces.push_back(&session.workspace(t));
        barriers.push_back(&session.barrier(t, team_size));
    }
    const kernels::Kernels& k = session.kernels();
    std::atomic<size_t> next{0};
    std::atomic<bool> stop{false};
    std::atomic<int64_t> skipped{0}, exited{0};
    std::mutex error_mutex;
    std::exception_ptr error;
    auto record_error = [&]() {
        std::lock_guard<std::mutex> lock(error_mutex);
        if (!error) error = std::current_exception();
        stop = true;
    };

    session.pool().run([&](int thread) {
        const int team_id = thread / team_size;
        if (team_id >= teams) return;  // threads left over when the pool does not divide into teams
        const Team team{thread % team_size, team_size, barriers[team_id]};
        TeamWorkspace& ws = *workspaces[team_id];
        for (;;) {
            if (team.leader()) {
                size_t idx = tasks.size();
                try {
                    // Take the next window the model has to see; the prefilter's rejects are recorded here.
                    while (!stop && (idx = next.fetch_add(1)) < tasks.size()) {
                        const Task& t = tasks[idx];
                        if (!prefilter ||
                            prefilter->keep(inputs[t.input].data, static_cast<int64_t>(inputs[t.input].size), t.start,
                                            t.length))
                            break;
                        result.inputs[t.input][t.index].flags |= kWindowSkipped;
                        ++skipped;
                    }
                    if (stop) idx = tasks.size();
                    if (idx < tasks.size()) {
                        const Task& t = tasks[idx];
                        const int T = t.length + (query ? m.query.region : 0);
                        if (query) {
                            ws.input.assign(prefix.begin(), prefix.end());
                            ws.input.insert(ws.input.end(), inputs[t.input].data + t.start,
                                            inputs[t.input].data + t.start + t.length);
                        }
                        prepare_workspace(m, ws, T, team.size);
                    }
                } catch (...) {
                    record_error();
                    idx = tasks.size();
                }
                ws.task = idx;
            }
            team.sync();
            const size_t idx = ws.task;
            if (idx >= tasks.size()) break;
            const Task& t = tasks[idx];
            const int T = t.length + (query ? m.query.region : 0);
            const uint8_t* bytes = query ? ws.input.data() : inputs[t.input].data + t.start;
            const float* hidden = forward_window(m, k, team, ws, bytes, T, 0, o.early_exit, nullptr);
            if (!team.leader()) continue;
            try {
                WindowResult& w = result.inputs[t.input][t.index];
                if (ws.failure) std::rethrow_exception(std::exchange(ws.failure, nullptr));
                if (!hidden) {
                    w.flags |= kWindowExited;
                    ++exited;
                    continue;
                }
                const int first = query ? m.query.region : 0;
                finish_window(m, k, hidden, T, first, t.start - first, o, w);
            } catch (...) {
                record_error();
            }
        }
    });
    if (error) std::rethrow_exception(error);
    result.skipped = skipped;
    result.exited = exited;
    return result;
}

}  // namespace pb
