#include "engine.h"

#include <algorithm>
#include <filesystem>
#include <thread>

#include "args.h"
#include "catalog.h"
#include "core/digest.h"
#include "detection.h"

namespace cli {

void check(pb_status status, const pb_error& err, const std::string& what) {
    if (status != PB_OK) throw UsageError(what + ": " + err.message, 2);
}

int default_threads() {
    const unsigned cores = std::thread::hardware_concurrency();
    return static_cast<int>(std::max(1u, cores > 1 ? cores - 1 : 1u));
}

namespace {

// The file is read once: the bytes that are hashed are the bytes that are loaded, so the file cannot be replaced
// between the checksum and the load.
ModelFile load_file(const std::string& path, const std::string& expected_sha256) {
    ModelFile m;
    m.path = display_path(path);
    std::vector<uint8_t> bytes = read_whole_file(path, "the model file");
    m.size = static_cast<int64_t>(bytes.size());
    m.sha256 = pb::sha256_hex(bytes.data(), bytes.size());
    if (!expected_sha256.empty() && expected_sha256 != "TBD" && expected_sha256 != m.sha256)
        throw UsageError(
            m.path +
                " does not match its published SHA-256: it is corrupt or was replaced. Run `purebyte models pull` "
                "again",
            2);
    pb_error err;
    pb_model* handle = nullptr;
    static const uint8_t kNothing = 0;  // an empty file is refused by the loader, not as a null pointer
    check(pb_model_load_memory(bytes.empty() ? &kNothing : bytes.data(), bytes.size(), &handle, &err), err,
          "cannot load " + m.path);
    m.handle.reset(handle, pb_model_free);
    return m;
}

// A model file rather than the name of a specialist: a path with a folder in it (`./model`, `models/x.gguf`), a name
// that starts with `.`, or one that ends in `.gguf`. Anything else is a catalog name, even when a file of that name
// exists in the current folder: the integrations run from the root of the repository they scan, which must not be
// able to replace `secrets-code` by shipping a file of that name.
bool looks_like_path(const std::string& spec) {
    return spec.find('/') != std::string::npos || spec.find('\\') != std::string::npos ||
           (!spec.empty() && spec[0] == '.') || (spec.size() > 5 && spec.compare(spec.size() - 5, 5, ".gguf") == 0);
}

}  // namespace

Specialist load_specialist(const std::string& spec, const std::vector<std::string>& extra_members, bool ensemble) {
    Specialist s;
    if (looks_like_path(spec)) {
        if (!file_exists(spec)) throw UsageError("model file not found: " + display_path(spec), 2);
        s.name = std::filesystem::u8path(spec).filename().u8string();
        s.members.push_back(load_file(spec, ""));
    } else {
        const Catalog catalog = Catalog::load();
        const CatalogEntry* entry = catalog.find(spec);
        if (!entry)
            throw UsageError("unknown model `" + spec +
                                 "`: give an installed specialist (see `purebyte models list`), or a model file by "
                                 "its path (a name ending in .gguf, or one with a folder such as ./my-model)",
                             2);
        require_runtime(*entry);
        s.name = entry->name;
        s.version = entry->version;
        s.profile = entry->profile;
        const std::string dir = models_directory();
        for (const CatalogFile& f : entry->files) {
            if (f.role != "model" && (f.role != "ensemble" || !ensemble)) continue;
            const std::string path = join_path(dir, f.file);
            if (!file_exists(path))
                throw UsageError("`" + entry->name + "` " + entry->version + " is not installed (missing " +
                                     display_path(path) + "): run `purebyte models pull " + entry->name + "`",
                                 2);
            ModelFile m = load_file(path, f.sha256);
            m.name = entry->name;
            m.version = entry->version;
            if (f.role == "model")
                s.members.insert(s.members.begin(), std::move(m));
            else
                s.members.push_back(std::move(m));
        }
        if (s.members.empty()) throw UsageError("the catalog entry of `" + entry->name + "` lists no model file", 2);
    }
    if (ensemble)
        for (const std::string& path : extra_members) {
            if (!file_exists(path)) throw UsageError("ensemble member not found: " + display_path(path), 2);
            s.members.push_back(load_file(path, ""));
        }
    for (ModelFile& m : s.members)
        if (m.name.empty()) m.name = std::filesystem::u8path(m.path).filename().u8string();
    return s;
}

std::shared_ptr<pb_session> make_session(int threads, int intra_threads, const std::string& kernel) {
    pb_session_options o;
    pb_session_options_init(&o);
    o.threads = threads;
    o.intra_threads = intra_threads;
    o.kernel = kernel.c_str();
    pb_session* session = nullptr;
    pb_error err;
    check(pb_session_create(&o, &session, &err), err, "cannot start the runtime");
    return std::shared_ptr<pb_session>(session, pb_session_free);
}

std::shared_ptr<pb_detector> make_detector(const Specialist& s, const std::string& profile) {
    std::vector<const pb_model*> models;
    for (const ModelFile& m : s.members) models.push_back(m.handle.get());
    // The caller's choice, else what the model file declares (pb_detector_create reads it), else the catalog's hint.
    std::string chosen = profile;
    if (chosen.empty() && declaration_of(models[0]).profile.empty()) chosen = s.profile;
    pb_detector* d = nullptr;
    pb_error err;
    check(pb_detector_create(models.data(), models.size(), chosen.empty() ? nullptr : chosen.c_str(), &d, &err), err,
          "cannot use `" + s.name + "`");
    return std::shared_ptr<pb_detector>(d, pb_detector_free);
}

}  // namespace cli
