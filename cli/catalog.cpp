#include "catalog.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "args.h"
#include "core/digest.h"
#include "core/files.h"
#include "core/json.h"
#include "purebyte/pb.h"

namespace cli {

const char* embedded_catalog();  // generated from models/models.json at build time

namespace {

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

// A catalog `file` is where `pull` writes inside the models directory, and what `verify` and --model read there: one
// plain name ending in .gguf (ASCII letters, digits, '.', '_' and '-', not starting with '.'), never a path, `..`, a
// drive or a stream, which would escape the directory.
bool plain_file_name(const std::string& name) {
    if (name.size() < 6 || name.size() > 200 || name[0] == '.' || name.compare(name.size() - 5, 5, ".gguf") != 0)
        return false;
    for (const char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == '-'))
            return false;
    return true;
}

std::vector<CatalogEntry> parse_catalog(const std::string& text, const std::string& origin) {
    pb::json::Value doc;
    std::string error;
    if (!pb::json::parse(text, doc, error))
        throw UsageError("the model catalog " + origin + " is not valid JSON: " + error);
    const pb::json::Value* schema = doc.get("schema");
    if (!schema || schema->number != 1) throw UsageError("the model catalog " + origin + " has an unknown schema");
    std::vector<CatalogEntry> out;
    const pb::json::Value* models = doc.get("models");
    if (!models) return out;
    auto text_of = [](const pb::json::Value& v, const char* key) {
        const pb::json::Value* x = v.get(key);
        return x && x->is(pb::json::Value::Type::String) ? x->string : std::string();
    };
    for (const pb::json::Value& m : models->array) {
        CatalogEntry e;
        e.name = text_of(m, "name");
        e.version = text_of(m, "version");
        e.summary = text_of(m, "summary");
        e.profile = text_of(m, "profile");
        e.min_runtime = text_of(m, "min_runtime");
        if (const pb::json::Value* files = m.get("files"))
            for (const pb::json::Value& f : files->array) {
                CatalogFile cf{text_of(f, "role"), text_of(f, "file"), text_of(f, "url"), text_of(f, "sha256"), -1};
                if (const pb::json::Value* size = f.get("size"))
                    if (size->is(pb::json::Value::Type::Number)) cf.size = static_cast<int64_t>(size->number);
                if (!plain_file_name(cf.file))
                    throw UsageError("the model catalog " + origin + " gives `" + e.name + "` the file `" +
                                         cf.file.substr(0, 200) +
                                         "`: a catalog file must be a plain name ending in .gguf (letters, digits, "
                                         "'.', '_' and '-'), never a path",
                                     2);
                e.files.push_back(cf);
            }
        if (!e.name.empty()) out.push_back(std::move(e));
    }
    return out;
}

}  // namespace

bool CatalogEntry::needs_newer_runtime() const {
    return !min_runtime.empty() && version_less(pb_version(), min_runtime);
}

void require_runtime(const CatalogEntry& entry) {
    if (entry.needs_newer_runtime())
        throw UsageError("`" + entry.name + "` " + entry.version + " needs purebyte " + entry.min_runtime +
                             " or newer, and this is purebyte " + pb_version() + ": upgrade purebyte",
                         2);
}

std::vector<uint8_t> read_whole_file(const std::string& path, const std::string& what) {
    try {
        return pb::read_file(path);
    } catch (const std::exception&) {
        throw UsageError("cannot read " + what + " " + display_path(path), 2);
    }
}

Catalog Catalog::load() {
    Catalog c;
    const std::string override_path = env("PUREBYTE_CATALOG");
    if (override_path.empty()) {
        c.entries_ = parse_catalog(embedded_catalog(), "built into purebyte");
    } else {
        const std::vector<uint8_t> bytes = read_whole_file(override_path, "the model catalog (PUREBYTE_CATALOG)");
        c.entries_ = parse_catalog(std::string(bytes.begin(), bytes.end()), display_path(override_path));
    }
    return c;
}

const CatalogEntry* Catalog::find(const std::string& spec) const {
    const size_t at = spec.find('@');
    const std::string name = spec.substr(0, at), version = at == std::string::npos ? "" : spec.substr(at + 1);
    const CatalogEntry *best = nullptr, *best_runnable = nullptr;
    for (const CatalogEntry& e : entries_) {
        if (e.name != name) continue;
        if (!version.empty()) {
            if (e.version == version) return &e;
            continue;
        }
        if (!best || version_less(best->version, e.version)) best = &e;
        if (!e.needs_newer_runtime() && (!best_runnable || version_less(best_runnable->version, e.version)))
            best_runnable = &e;
    }
    return best_runnable ? best_runnable : best;
}

bool version_less(const std::string& a, const std::string& b) {
    const std::vector<std::string> x = split_list(a, '.'), y = split_list(b, '.');
    for (size_t i = 0; i < std::max(x.size(), y.size()); ++i) {
        const long p = i < x.size() ? std::strtol(x[i].c_str(), nullptr, 10) : 0;
        const long q = i < y.size() ? std::strtol(y[i].c_str(), nullptr, 10) : 0;
        if (p != q) return p < q;
    }
    return false;
}

std::string models_directory() {
    const std::string home = env("PUREBYTE_HOME");
    if (!home.empty()) return home;
#ifdef _WIN32
    const std::string base = env("LOCALAPPDATA");
    if (!base.empty()) return base + "\\purebyte\\models";
    const char* missing = "LOCALAPPDATA is not set";
#elif defined(__APPLE__)
    const std::string base = env("HOME");
    if (!base.empty()) return base + "/Library/Application Support/purebyte/models";
    const char* missing = "HOME is not set";
#else
    const std::string data = env("XDG_DATA_HOME");
    if (!data.empty()) return data + "/purebyte/models";
    const std::string base = env("HOME");
    if (!base.empty()) return base + "/.local/share/purebyte/models";
    const char* missing = "neither XDG_DATA_HOME nor HOME is set";
#endif
    // Never a folder relative to the current one, which is often the repository being scanned.
    throw UsageError(std::string("no models directory: ") + missing + "; set PUREBYTE_HOME to the folder of the models",
                     2);
}

std::string join_path(const std::string& directory, const std::string& file) {
    return (std::filesystem::u8path(directory) / std::filesystem::u8path(file)).u8string();
}

std::string display_path(std::string path) {
#ifdef _WIN32
    std::replace(path.begin(), path.end(), '\\', '/');
#endif
    return path;
}

bool file_exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(std::filesystem::u8path(path), ec);
}

std::string file_sha256(const std::string& path, int64_t* size) {
    std::FILE* f = pb::open_file(path, "rb");
    if (!f) return "";
    pb::Sha256 hash;
    std::vector<uint8_t> buffer(1 << 20);
    int64_t total = 0;
    for (size_t got; (got = std::fread(buffer.data(), 1, buffer.size(), f)) > 0;) {
        hash.update(buffer.data(), got);
        total += static_cast<int64_t>(got);
    }
    std::fclose(f);
    if (size) *size = total;
    return hash.hex();
}

std::string recorded_sha256(const std::string& path) {
    std::FILE* f = pb::open_file(path + ".sha256", "rb");
    if (!f) return "";
    char buffer[65] = {0};
    const size_t got = std::fread(buffer, 1, 64, f);
    std::fclose(f);
    return got == 64 ? std::string(buffer, 64) : "";
}

void write_sha256_file(const std::string& path, const std::string& hex) {
    std::FILE* f = pb::open_file(path + ".sha256", "wb");
    if (!f) throw UsageError("cannot write " + display_path(path) + ".sha256", 2);
    const std::string line = hex + "  " + std::filesystem::u8path(path).filename().u8string() + "\n";
    const bool written = std::fwrite(line.data(), 1, line.size(), f) == line.size();
    if (std::fclose(f) != 0 || !written) throw UsageError("cannot write " + display_path(path) + ".sha256", 2);
}

}  // namespace cli
