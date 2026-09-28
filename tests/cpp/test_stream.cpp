// The stream context (spec/FORMAT.md, section 12.4): the hidden states of the chunks are bit for bit those of one
// forward pass over the whole input, whatever the chunk size and the threads, and the heads of each chunk are the
// heads run on that slice of the whole-input hidden states.
#include <cstring>
#include <string>
#include <vector>

#include "core/failure.h"
#include "model/model.h"
#include "runtime/encode.h"
#include "runtime/forward.h"
#include "runtime/scan.h"
#include "runtime/session.h"
#include "test.h"

TEST(stream_chunks_are_one_forward_pass) {
    const std::string path = pbtest::model_path("stream-context");
    if (path.empty()) return;
    const auto model = pb::load_model(path);
    CHECK(model->streaming);
    pbtest::Random rng(11);
    std::vector<uint8_t> bytes(301);
    for (uint8_t& b : bytes) b = static_cast<uint8_t>(rng.below(3) ? 'a' + rng.below(26) : rng.below(256));
    const pb::Bytes input{bytes.data(), bytes.size()};
    pb::Session one(1, 0, "auto");
    const std::vector<float> whole = pb::encode(one, *model, input);
    const int d = model->d_model;

    for (int chunk : {40, 17, 3, 301, 500})
        for (int threads : {1, 3}) {
            pb::Session session(threads, threads, "auto");
            pb::ScanOptions o;
            o.window = chunk;
            o.digest = true;
            o.ungated = true;
            const pb::ScanResult r = pb::scan(session, *model, {input}, o);
            const std::vector<pb::WindowResult>& chunks = r.inputs[0];
            CHECK(chunks.size() == (bytes.size() + chunk - 1) / chunk);
            int64_t covered = 0;
            for (const pb::WindowResult& w : chunks) {
                CHECK(w.start == covered);
                covered += w.length;
                // The heads on the slice of the whole-input hidden states give the same outputs and digest.
                const float* slice = whole.data() + static_cast<size_t>(w.start) * d;
                std::vector<pb::HeadOutput> heads;
                pb::run_heads(*model, session.kernels(), slice, w.length, 0, o.bias, true, heads);
                const uint64_t digest = pb::window_digest(slice, static_cast<size_t>(w.length) * d, heads);
                CHECK_MSG(digest == w.digest, "chunk " + std::to_string(chunk) + " at " + std::to_string(w.start));
            }
            CHECK(covered == static_cast<int64_t>(bytes.size()));
        }
}

TEST(stream_skips_short_inputs_and_refuses_window_options) {
    const std::string path = pbtest::model_path("stream-context");
    if (path.empty()) return;
    const auto model = pb::load_model(path);
    pb::Session session(2, 0, "auto");
    std::vector<uint8_t> tiny(5, 'x'), empty;
    pb::ScanOptions o;
    const pb::ScanResult r = pb::scan(session, *model, {{tiny.data(), tiny.size()}, {empty.data(), 0}}, o);
    CHECK(r.inputs.size() == 2 && r.inputs[0].empty() && r.inputs[1].empty());  // below purebyte.window.min = 8
    o.stride = 7;
    CHECK_FAILS(pb::scan(session, *model, {{tiny.data(), tiny.size()}}, o), PB_ERR_ARGUMENT);
    o.stride = 0;
    o.prefilter = true;
    CHECK_FAILS(pb::scan(session, *model, {{tiny.data(), tiny.size()}}, o), PB_ERR_ARGUMENT);
}
