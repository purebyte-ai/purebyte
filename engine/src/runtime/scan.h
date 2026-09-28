// Runs a model over every window of a batch of inputs, in parallel on a session's threads, and keeps what every head
// produced in every window.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "heads/head.h"
#include "model/model.h"
#include "runtime/session.h"

namespace pb {

struct Bytes {
    const uint8_t* data = nullptr;
    size_t size = 0;
};

struct ScanOptions {
    int window = 0;      // bytes per window; 0 = the model's
    int stride = 0;      // 0 = the model's
    int min_length = 0;  // shortest window evaluated; 0 = the model's
    bool whole = false;  // one window per input, whatever its length
    bool prefilter = false;
    std::string prefilter_rule;  // empty = Prefilter::kDefaultRule
    bool early_exit = false;
    bool digest = false;
    bool ungated = false;
    SpanBias bias;
    std::vector<std::string> queries;  // query-conditioned models
};

enum WindowFlag : uint32_t {
    kWindowSkipped = 1,  // the prefilter skipped it
    kWindowExited = 2,   // the exit head stopped it
    kWindowGated = 4,    // a gate said negative
};

struct WindowResult {
    int64_t start = 0;
    int32_t length = 0;  // bytes of the input covered (without a query prefix)
    uint32_t flags = 0;
    uint64_t digest = 0;
    int32_t label = 0;              // first choice head: argmax (0 when not computed)
    float p_positive = 0.f;         // first choice head: 1 - P(class 0)
    std::vector<HeadOutput> heads;  // one per head; spans in input offsets
};

struct ScanResult {
    std::vector<std::vector<WindowResult>> inputs;
    int window = 0, stride = 0, min_length = 0;  // the values used
    int64_t skipped = 0, exited = 0;
};

// Windows of `window` bytes every `stride` bytes (the model's unless the options say otherwise), or, for a model with
// the stream context, every input as one sequence (see scan_streams).
ScanResult scan(Session& session, const Model& model, const std::vector<Bytes>& inputs, const ScanOptions& options);

// Stream context (`purebyte.context = stream`): every input is ONE sequence, run in consecutive chunks of `window`
// bytes with the blocks' state carried from chunk to chunk, so that the hidden states are bit for bit those of a
// single forward over the whole input. The heads run on each chunk, which is reported as a window. An input shorter
// than `min_length` has no chunk; a short last chunk is evaluated (it has the whole input to its left).
ScanResult scan_streams(Session& session, const Model& model, const std::vector<Bytes>& inputs,
                        const ScanOptions& options);

// The leader's work after the body of a window: runs the heads, then fills `w` (outputs, gated flag, digest, label and
// p_positive of the first choice head) and moves its spans by `offset` into input coordinates.
void finish_window(const Model& model, const kernels::Kernels& kernels, const float* hidden, int T, int first_position,
                   int64_t offset, const ScanOptions& options, WindowResult& w);

}  // namespace pb
