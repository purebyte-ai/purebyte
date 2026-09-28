// A loaded model: everything the file declares, validated and ready to run. Immutable after loading; shared by any
// number of threads and sessions.
//
//   bytes -> embedding (+ n-gram tables at their layer) -> blocks -> final RMSNorm (-> lookahead) -> heads
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "blocks/block.h"
#include "format/gguf.h"
#include "heads/head.h"
#include "model/lookahead.h"
#include "model/ngram.h"

namespace pb {

constexpr int kFormatVersion = 3;  // newest format version this library reads (spec/FORMAT.md)

// Bounds on what one window of the declared size may cost (spec/FORMAT.md, section 13): the shapes come from the
// file, and a small file must not declare a window that allocates gigabytes or runs for hours.
constexpr int kMaxWindowSize = 1 << 20;             // purebyte.window.size
constexpr int64_t kMaxWindowWidthFloats = 1 << 24;  // window.size x the widest row a window allocates per position
constexpr int kMaxAttentionWindow = 1 << 13;        // window.size of a model with attention blocks (T^2 work)

// How a document longer than a window is cut: windows of `size` bytes every `stride` bytes; windows shorter than
// `min_length` bytes are not evaluated. With a query template, `stride` and `min_length` count document bytes, of
// which a window holds size - region.
struct WindowPolicy {
    int size = 512;
    int stride = 384;
    int min_length = 24;

    int document_bytes(int query_region) const { return size - query_region; }
};

// Query-conditioned models: every window starts with a prefix region of `region` bytes holding the queries, each
// written as prefix + query + suffix, the rest padded with `pad`; the document fills the rest of the window.
struct QueryTemplate {
    int max_queries = 0;
    int region = 0;
    std::string prefix = "?";
    std::string suffix = "\n";
    uint8_t pad = '\n';

    bool enabled() const { return region > 0; }
    // The `region` prefix bytes for these queries; PB_ERR_ARGUMENT when they do not fit.
    std::vector<uint8_t> encode(const std::vector<std::string>& queries) const;
};

struct Model {
    std::shared_ptr<const gguf::File> file;
    int format_version = 0;
    std::string name;
    int d_model = 0;
    const float* embedding = nullptr;  // [256, d_model]
    std::unique_ptr<NGramInput> ngram;
    std::vector<std::unique_ptr<Block>> blocks;
    const float* final_norm = nullptr;  // [d_model]
    std::unique_ptr<Lookahead> lookahead;
    std::vector<std::unique_ptr<Head>> heads;
    const ExitHead* exit_head = nullptr;
    int first_choice_head = -1;
    WindowPolicy window;
    QueryTemplate query;
    std::string profile;      // post-processing the model declares; empty = none declared
    bool streaming = false;   // purebyte.context = "stream": the input is one sequence with carried state
    std::string description;  // JSON (pb_model_describe)

    // Scratch the forward of a window of T positions needs: shared by a team, and private to each member.
    size_t scratch_floats(int T) const;
    size_t member_scratch_floats(int T) const;
    // Floats of carried state for streaming; 0 when some block cannot stream.
    size_t stream_state_floats() const;
    int find_head(const std::string& type) const;
};

std::shared_ptr<const Model> load_model(const std::string& path);
std::shared_ptr<const Model> load_model_memory(const void* data, size_t size);

}  // namespace pb
