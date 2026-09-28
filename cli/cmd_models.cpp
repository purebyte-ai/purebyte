// purebyte models list | pull NAME[@VERSION] | verify [NAME]: the released specialists and their checksums.
// `pull` is the only command that uses the network: it downloads with curl (HTTPS, or file:// for local mirrors) and
// refuses a file whose SHA-256 differs from the catalog's.
#include <cstdio>
#include <filesystem>

#include "args.h"
#include "catalog.h"
#include "child_process.h"
#include "commands.h"
#include "core/json.h"

namespace cli {

namespace {

namespace fs = std::filesystem;

enum class FileState { Missing, Verified, Unverified, Corrupt };

FileState check_file(const std::string& path, const CatalogFile& f) {
    if (!file_exists(path)) return FileState::Missing;
    int64_t size = 0;
    const std::string sha = file_sha256(path, &size);
    const bool published = !f.sha256.empty() && f.sha256 != "TBD";
    const std::string recorded = recorded_sha256(path);
    if ((published && sha != f.sha256) || (!recorded.empty() && sha != recorded) || (f.size >= 0 && size != f.size))
        return FileState::Corrupt;
    return published || !recorded.empty() ? FileState::Verified : FileState::Unverified;
}

const char* state_name(FileState s) {
    switch (s) {
        case FileState::Missing: return "not installed";
        case FileState::Verified: return "installed";
        case FileState::Unverified: return "installed (no checksum to verify)";
        case FileState::Corrupt: return "CORRUPT (checksum mismatch)";
    }
    return "?";
}

FileState entry_state(const CatalogEntry& e, const std::string& dir) {
    FileState worst = FileState::Verified;
    for (const CatalogFile& f : e.files) {
        const FileState s = check_file(join_path(dir, f.file), f);
        if (s == FileState::Corrupt) return s;
        if (s == FileState::Missing)
            worst = FileState::Missing;
        else if (s == FileState::Unverified && worst == FileState::Verified)
            worst = s;
    }
    return worst;
}

// The state `list` shows: that of its files, or that this runtime is too old for it.
std::string entry_state_text(const CatalogEntry& e, const std::string& dir) {
    if (e.needs_newer_runtime()) return "needs purebyte " + e.min_runtime + " or newer";
    return state_name(entry_state(e, dir));
}

int list(const Args& a) {
    if (a.positional().size() > 2) throw UsageError("`models list` takes no argument");
    if (a.has("force")) throw UsageError("--force belongs to `models pull`");
    const Catalog catalog = Catalog::load();
    const std::string dir = models_directory();
    if (a.has("json")) {
        pb::json::Writer w;
        w.begin_object().field("directory", display_path(dir)).key("models").begin_array();
        for (const CatalogEntry& e : catalog.entries())
            w.begin_object()
                .field("name", e.name)
                .field("version", e.version)
                .field("state", entry_state_text(e, dir))
                .field("summary", e.summary)
                .end_object();
        w.end_array().end_object();
        std::printf("%s\n", w.str().c_str());
        return 0;
    }
    std::printf("models directory: %s\n\n", display_path(dir).c_str());
    std::printf("%-16s %-10s %-34s %s\n", "NAME", "VERSION", "STATE", "SUMMARY");
    for (const CatalogEntry& e : catalog.entries())
        std::printf("%-16s %-10s %-34s %s\n", e.name.c_str(), e.version.c_str(), entry_state_text(e, dir).c_str(),
                    e.summary.c_str());
    return 0;
}

int pull(const Args& a) {
    if (a.positional().size() != 3) throw UsageError("usage: purebyte models pull NAME[@VERSION]");
    if (a.has("json")) throw UsageError("--json belongs to `models list`");
    const std::string spec = a.positional()[2];
    const Catalog catalog = Catalog::load();  // refuses a catalog whose file names are not plain names
    const CatalogEntry* e = catalog.find(spec);
    if (!e) throw UsageError("unknown model `" + spec + "` (see `purebyte models list`)", 2);
    require_runtime(*e);
    const std::string dir = models_directory();
    std::error_code ec;
    fs::create_directories(fs::u8path(dir), ec);
    if (ec) throw UsageError("cannot create the models directory " + display_path(dir) + ": " + ec.message(), 2);
    for (const CatalogFile& f : e->files) {
        const std::string path = join_path(dir, f.file);
        if (!a.has("force") && check_file(path, f) == FileState::Verified) {
            std::fprintf(stderr, "%s: already installed and verified\n", f.file.c_str());
            continue;
        }
        if (f.url.empty() || f.url == "TBD" || f.sha256.empty() || f.sha256 == "TBD")
            throw UsageError(
                e->name + " " + e->version + " is not published yet (no download URL or checksum in the catalog)", 2);
        const std::string partial = path + ".part";
        std::fprintf(stderr, "downloading %s\n", f.url.c_str());
        const ProcessResult r = run_process({"curl", "--fail", "--location", "--silent", "--show-error", "--proto",
                                             "=https,file", "--output", partial, f.url});
        if (r.exit_code == -1) throw UsageError("curl is needed to download models: " + r.errors, 2);
        if (r.exit_code != 0) {
            fs::remove(fs::u8path(partial), ec);
            throw UsageError("download failed: " + r.errors, 2);
        }
        int64_t size = 0;
        const std::string sha = file_sha256(partial, &size);
        if (sha != f.sha256 || (f.size >= 0 && size != f.size)) {
            fs::remove(fs::u8path(partial), ec);
            throw UsageError(f.file + ": the download does not match the published SHA-256; nothing was installed", 2);
        }
        fs::rename(fs::u8path(partial), fs::u8path(path), ec);
        if (ec) throw UsageError("cannot install " + display_path(path) + ": " + ec.message(), 2);
        write_sha256_file(path, sha);
        std::fprintf(stderr, "%s: installed and verified (%lld bytes)\n", f.file.c_str(), static_cast<long long>(size));
    }
    return 0;
}

// `verify` checks every installed file; `verify NAME` checks the files of that specialist (the version `pull` would
// install, or the one given), and a file of it that is missing is a problem too.
int verify(const Args& a) {
    if (a.positional().size() > 3) throw UsageError("usage: purebyte models verify [NAME[@VERSION]]");
    if (a.has("json") || a.has("force")) throw UsageError("`models verify` takes no option but --help");
    const Catalog catalog = Catalog::load();
    const std::string dir = models_directory();
    const std::string only = a.positional().size() > 2 ? a.positional()[2] : "";
    const CatalogEntry* named = only.empty() ? nullptr : catalog.find(only);
    if (!only.empty() && !named) throw UsageError("unknown model `" + only + "` (see `purebyte models list`)", 2);
    int problems = 0, checked = 0;
    for (const CatalogEntry& e : catalog.entries()) {
        if (named && &e != named) continue;
        for (const CatalogFile& f : e.files) {
            const FileState s = check_file(join_path(dir, f.file), f);
            if (s == FileState::Missing) {
                if (named) {
                    ++problems;
                    std::printf("%-44s %s\n", f.file.c_str(), "MISSING");
                }
                continue;
            }
            ++checked;
            problems += s == FileState::Corrupt;
            std::printf("%-44s %s\n", f.file.c_str(),
                        s == FileState::Corrupt    ? "FAILED"
                        : s == FileState::Verified ? "OK"
                                                   : "no checksum");
        }
    }
    if (named && problems)
        std::fprintf(stderr,
                     "purebyte: `%s` %s is not installed completely and correctly: run `purebyte models pull %s`\n",
                     named->name.c_str(), named->version.c_str(), only.c_str());
    // Files copied by hand next to their .sha256 (offline installation) that the catalog does not list.
    std::error_code ec;
    for (fs::directory_iterator it(fs::u8path(dir), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string path = it->path().u8string();
        if (it->path().extension() != ".gguf" || !only.empty()) continue;
        bool known = false;
        for (const CatalogEntry& e : catalog.entries())
            for (const CatalogFile& f : e.files) known = known || f.file == it->path().filename().u8string();
        if (known) continue;
        const std::string recorded = recorded_sha256(path);
        if (recorded.empty()) continue;
        ++checked;
        const bool ok = file_sha256(path) == recorded;
        problems += !ok;
        std::printf("%-44s %s\n", it->path().filename().u8string().c_str(), ok ? "OK" : "FAILED");
    }
    if (checked == 0 && !named) std::printf("no installed model to verify in %s\n", display_path(dir).c_str());
    return problems ? 2 : 0;
}

}  // namespace

int command_models(const std::vector<std::string>& argv) {
    const std::vector<OptionSpec> spec = {
        {"json", nullptr, "list: machine-readable output"},
        {"force", nullptr, "pull: download again even when installed"},
        {"help", nullptr, "show this help"},
    };
    const Args a(argv, 1, spec);
    const std::string action = a.positional().size() > 1 ? a.positional()[1] : "";
    if (a.has("help") || action.empty()) {
        std::fputs(
            ("usage: purebyte models list | pull NAME[@VERSION] | verify [NAME]\n\nModels live in PUREBYTE_HOME or "
             "in the per-user directory shown by `purebyte models list`.\n\n" +
             options_help(spec))
                .c_str(),
            stdout);
        return action.empty() && !a.has("help") ? 2 : 0;
    }
    if (action == "list") return list(a);
    if (action == "pull") return pull(a);
    if (action == "verify") return verify(a);
    throw UsageError("unknown models action `" + action + "` (list, pull or verify)");
}

}  // namespace cli
