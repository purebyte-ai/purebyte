// The catalog of released specialists (models/models.json, compiled into the binary) and the models directory.
//
// Catalog schema 1: {"schema": 1, "models": [{"name", "version", "summary", "profile"?, "min_runtime"?, "files":
// [{"role": "model" | "ensemble", "file", "url", "sha256", "size"}], ...}]}. PUREBYTE_CATALOG may point to another
// catalog file. Models live in PUREBYTE_HOME, or in a per-user directory; a file is installed as its catalog `file`
// name, with a `<file>.sha256` next to it ("<hex>  <file>", the format of sha256sum). A catalog whose `file` is not a
// plain file name ending in .gguf (a path, `..`, a drive) is refused: `pull` writes there.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cli {

struct CatalogFile {
    std::string role;  // "model" or "ensemble"
    std::string file;
    std::string url;     // "TBD" until published
    std::string sha256;  // "TBD" until published
    int64_t size = -1;
};

struct CatalogEntry {
    std::string name, version, summary, profile;
    std::string min_runtime;  // the oldest purebyte that can run it ("" = any)
    std::vector<CatalogFile> files;
    // Whether this runtime is older than `min_runtime`.
    bool needs_newer_runtime() const;
};

class Catalog {
public:
    // The embedded catalog, or the file named by PUREBYTE_CATALOG.
    static Catalog load();
    const std::vector<CatalogEntry>& entries() const { return entries_; }
    // `name` or `name@version`; without a version, the highest one this runtime can run (else the highest, which the
    // caller refuses with require_runtime). nullptr when unknown.
    const CatalogEntry* find(const std::string& spec) const;

private:
    std::vector<CatalogEntry> entries_;
};

// Throws UsageError (exit 2) when `entry` needs a newer purebyte than this one.
void require_runtime(const CatalogEntry& entry);

// PUREBYTE_HOME, else the per-user data directory (created on demand by `pull`). Throws UsageError when neither can
// be found (no PUREBYTE_HOME, and no LOCALAPPDATA, HOME or XDG_DATA_HOME): never a folder relative to the current one.
std::string models_directory();
std::string join_path(const std::string& directory, const std::string& file);
// A path as the CLI writes it in reports and messages: as given (relative stays relative), with `/` as the separator
// on every platform (Windows `\` becomes `/`). Files are still opened through their native path.
std::string display_path(std::string path);
bool file_exists(const std::string& path);

// SHA-256 of a file, streamed; empty when it cannot be read.
std::string file_sha256(const std::string& path, int64_t* size = nullptr);
// The hash recorded in `<path>.sha256`, or empty.
std::string recorded_sha256(const std::string& path);
// Throws UsageError (exit 2) when the file cannot be written completely.
void write_sha256_file(const std::string& path, const std::string& hex);
// A whole file; throws UsageError (exit 2) naming it (as display_path) when it cannot be read. `what` describes it:
// "the model catalog", "the spans file"...
std::vector<uint8_t> read_whole_file(const std::string& path, const std::string& what);

// Version order of "MAJOR.MINOR.PATCH" strings (numeric per part).
bool version_less(const std::string& a, const std::string& b);

}  // namespace cli
