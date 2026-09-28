// The CLI's side of the C API: owning wrappers for models, sessions and detectors, and the resolution of `--model`.
// Everything that runs a model goes through pb.h.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "purebyte/pb.h"

namespace cli {

struct ModelFile {
    std::string path;  // as displayed (display_path); files are opened through the path they were loaded from
    std::string sha256, name, version;
    int64_t size = 0;
    std::shared_ptr<pb_model> handle;
};

// A specialist as the user named it: a catalog name (its model file plus ensemble members) or one model file.
struct Specialist {
    std::string name;                // catalog name, or the file name
    std::string version;             // catalog version, or ""
    std::string profile;             // the catalog's profile hint ("" = let the model declare it)
    std::vector<ModelFile> members;  // [0] is the main model
};

// Resolves and loads `spec` (a catalog name, name@version, or a .gguf path) plus explicit extra ensemble files.
// `ensemble` = false loads only the main model. Throws UsageError with an actionable message.
Specialist load_specialist(const std::string& spec, const std::vector<std::string>& extra_members, bool ensemble);

std::shared_ptr<pb_session> make_session(int threads, int intra_threads, const std::string& kernel);
std::shared_ptr<pb_detector> make_detector(const Specialist& s, const std::string& profile);

// Default worker threads: the hardware threads (logical CPUs) of the machine minus one (at least one).
int default_threads();

// Throws UsageError(message, exit 2) for a failed C API call.
void check(pb_status status, const pb_error& err, const std::string& what);

}  // namespace cli
