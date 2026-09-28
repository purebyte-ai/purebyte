#include "inputs.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>

#include "args.h"
#include "catalog.h"
#include "child_process.h"
#include "core/files.h"
#include "exclusions.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace cli {

namespace {

std::vector<uint8_t> read_stdin() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    std::vector<uint8_t> content;
    uint8_t chunk[1 << 16];
    for (size_t got; (got = std::fread(chunk, 1, sizeof chunk, stdin)) > 0;)
        content.insert(content.end(), chunk, chunk + got);
    return content;
}

bool read_disk_file(const std::string& path, std::vector<uint8_t>& out) {
    try {
        out = pb::read_file(path);
        return true;
    } catch (...) {
        return false;
    }
}

// The library's wording for an input over the profile's limit (checked here before reading: a huge file is never
// loaded just to be refused), in exact bytes. `model_limit` = false for a command's own limit (decide, --spans).
std::string over_limit(uint64_t size, uint64_t limit, bool model_limit = true) {
    return std::to_string(size) + " bytes, over " + (model_limit ? "this model's limit" : "the limit") + " of " +
           std::to_string(limit) + " bytes: not scanned";
}

// `parent` is a display path (display_path): its separator is '/'.
std::string join_display(const std::string& parent, const std::string& name) {
    if (parent.empty()) return name;
    return parent.back() == '/' ? parent + name : parent + "/" + name;
}

// A file named on the command line or met by a walk: its display name, where it is on disk, and its path for the
// profile's path rules (Source::path).
struct Listed {
    std::string display;
    fs::path disk;
    std::string rule;
    bool operator<(const Listed& o) const { return display != o.display ? display < o.display : disk < o.disk; }
};

// What walks keep: the files the profile wants, minus (with --tests=no) those whose findings would be warnings.
struct WalkPolicy {
    const pb_detector* detector = nullptr;
    bool skip_warnings = false;
    bool keeps(const std::string& rule) const {
        return !skip_warnings || pb_detector_path_severity(detector, rule.c_str()) != PB_SEVERITY_WARNING;
    }
};

// An absolute (or drive- or share-relative) path, or one that climbs out of the working directory: the folders it
// names are not part of the project scanned.
bool outside_project(const fs::path& path) {
    if (path.has_root_name() || path.has_root_directory()) return true;
    const fs::path normal = path.lexically_normal();
    return !normal.empty() && *normal.begin() == "..";
}

// A link, never followed by a walk or a diff: a symbolic link, or on Windows any reparse point that stands for another
// file or folder (a junction, a symbolic link, a WSL link: the "name surrogate" tags). std::filesystem does not report
// junctions as links, and one that points to a folder above it would make a walk endless. Other reparse points, such
// as the placeholders of files kept in the cloud, are ordinary files and folders.
bool is_link(const fs::path& path) {
    std::error_code e;
    if (fs::is_symlink(fs::symlink_status(path, e))) return true;
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    WIN32_FIND_DATAW data;
    const HANDLE found = FindFirstFileW(path.c_str(), &data);
    if (found == INVALID_HANDLE_VALUE) return true;  // a reparse point that cannot be identified: not followed
    FindClose(found);
    return IsReparseTagNameSurrogate(data.dwReserved0);
#else
    return false;
#endif
}

// A directory walk, top-down, entering neither links (symbolic links, and junctions on Windows) nor the directories the
// profile skips or the user excludes. `rule` is the directory's own path for the path rules ("" at the top of an
// argument outside the project); `top` is its path from the top of the walk, which exclusion patterns match.
void walk(const fs::path& directory, const std::string& display, const std::string& rule, const std::string& top,
          const WalkPolicy& policy, const Exclusions& exclusions, std::vector<Listed>& files,
          std::vector<Failure>& failures, int depth) {
    if (depth > 128) {
        failures.push_back({display, "more than 128 nested folders (a link cycle?): not walked"});
        return;
    }
    std::error_code ec;
    fs::directory_iterator it(directory, ec), end;
    if (ec) {
        failures.push_back({display, "could not be listed: " + ec.message()});
        return;
    }
    struct Sub {
        Listed listed;
        std::string top;
    };
    std::vector<Sub> subdirectories;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        const fs::directory_entry& entry = *it;
        const std::string name = entry.path().filename().u8string();
        const std::string entry_top = join_display(top, name);
        std::error_code e;
        if (entry.is_directory(e)) {
            if (pb_detector_wants_directory(policy.detector, name.c_str()) && !is_link(entry.path()) &&
                !exclusions.excluded(entry_top, true))
                subdirectories.push_back(
                    {{join_display(display, name), entry.path(), join_display(rule, name)}, entry_top});
        } else if (pb_detector_wants_file(policy.detector, name.c_str()) && !is_link(entry.path()) &&
                   !exclusions.excluded(entry_top, false)) {
            const std::string file_rule = join_display(rule, name);
            if (policy.keeps(file_rule)) files.push_back({join_display(display, name), entry.path(), file_rule});
        }
    }
    for (const Sub& sub : subdirectories)
        walk(sub.listed.disk, sub.listed.display, sub.listed.rule, sub.top, policy, exclusions, files, failures,
             depth + 1);
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

// The command line of git, with the paths it is given taken literally (--literal-pathspecs: a file named `[ab].py` or
// `:(glob)app.env` is that file, never a pattern that matches others), and without the settings of the repository
// that would run a program or change what a diff says: no file system monitor, paths from the root of the repository,
// renames paired (never copies). `args` start with "-C", the repository and the git command.
std::vector<std::string> git_command(const std::vector<std::string>& args) {
    std::vector<std::string> argv = {"git", "--literal-pathspecs", "-c", "core.fsmonitor=false",
                                     "-c",  "diff.relative=false", "-c", "diff.renames=true"};
    argv.insert(argv.end(), args.begin(), args.end());
    return argv;
}

std::string git(const std::vector<std::string>& args) {
    const ProcessResult r = run_process(git_command(args));
    if (r.exit_code == -1) throw UsageError("git is needed for --staged and --git-diff: " + trim(r.errors), 2);
    if (r.exit_code != 0) throw UsageError("git " + args[2] + " failed: " + trim(r.errors), 2);
    return r.output;
}

// One file of `git diff --raw -z --no-abbrev`: records ":<old mode> <new mode> <old id> <new id> <status>[score]\0"
// followed by the path, or for a rename (R) or a copy (C) the old path and then the new one.
struct Change {
    std::string mode;      // of the new side: 100644, 100755, 120000 (a link), 160000 (a submodule)
    std::string id;        // of the new side: the staged blob with --cached; zeros for a file of the working tree
    std::string path;      // the new path, from the root of the repository
    std::string old_path;  // renames: the path it had
};

std::vector<Change> raw_changes(const std::string& raw) {
    std::vector<Change> out;
    size_t at = 0;
    const auto field = [&raw, &at](std::string& value) {
        const size_t zero = raw.find('\0', at);
        if (zero == std::string::npos) return false;
        value = raw.substr(at, zero - at);
        at = zero + 1;
        return true;
    };
    std::string head;
    while (at < raw.size()) {
        const std::vector<std::string> parts = field(head) ? split_list(head, ' ') : std::vector<std::string>();
        if (parts.size() != 5 || parts[0].empty() || parts[0][0] != ':' || parts[4].empty())
            throw UsageError("git diff --raw printed a line this version of purebyte does not understand", 2);
        Change c{parts[1], parts[3], "", ""};
        const char status = parts[4][0];
        if ((status == 'R' || status == 'C') && !field(c.old_path))
            throw UsageError("git diff --raw printed a rename without its paths", 2);
        if (!field(c.path)) throw UsageError("git diff --raw printed a change without its path", 2);
        out.push_back(std::move(c));
    }
    return out;
}

// The lines a unified diff with no context (-U0) adds: from "@@ -a,b +c,d @@", lines c .. c+d-1. A binary file has
// no lines: all of it counts.
AddedLines added_lines(const std::string& diff) {
    AddedLines added;
    if (diff.find("\nBinary files ") != std::string::npos || diff.rfind("Binary files ", 0) == 0) return added;
    added.all = false;
    for (size_t at = diff.find("\n@@ "); at != std::string::npos; at = diff.find("\n@@ ", at + 1)) {
        const size_t plus = diff.find(" +", at);
        if (plus == std::string::npos) break;
        char* end = nullptr;
        const long start = std::strtol(diff.c_str() + plus + 2, &end, 10);
        long count = 1;
        if (*end == ',') count = std::strtol(end + 1, nullptr, 10);
        if (count > 0) added.ranges.push_back({start, start + count - 1});
    }
    return added;
}

// Files added, copied, modified, renamed or whose type changed (a link that became a file), from `git diff --raw`
// (one listing: modes, staged object ids and rename pairs). Submodules are other repositories and are not entered.
// Each file's added lines come from its own patch, with the old path of a rename next to the new one so that git pairs
// them: a file that is only moved adds no line. Textconv filters and external diff programs are not run, so the line
// numbers are those of the file that is scanned. Sizes are known before anything is read: an empty file is skipped, and
// one over the limit is reported without being read, as a walk does.
InputPlan plan_git(const std::vector<std::string>& diff_args, const std::string& path, bool staged,
                   const WalkPolicy& policy, const std::vector<std::string>& excludes) {
    const pb_detector* detector = policy.detector;
    InputPlan plan;
    const uint64_t limit = pb_detector_max_input_bytes(detector);
    const std::string root = trim(git({"-C", path, "rev-parse", "--show-toplevel"}));
    Exclusions exclusions;  // --exclude, then the .purebyteignore of the repository: paths from its root
    for (const std::string& e : excludes) exclusions.add(e);
    exclusions.load((fs::u8path(root) / ".purebyteignore").u8string());
    std::vector<std::string> list = {"-C", path, "diff", "--raw", "-z", "--no-abbrev", "--diff-filter=ACMRT"};
    list.insert(list.end(), diff_args.begin(), diff_args.end());
    list.insert(list.end(), {"--", "."});
    for (const Change& change : raw_changes(git(list))) {
        const std::string& name = change.path;
        if (change.mode == "160000") continue;  // a submodule (gitlink): another repository, not a file of this one
        if (name.empty() || !pb_detector_wants_file(detector, fs::u8path(name).filename().u8string().c_str()) ||
            !policy.keeps(name) || exclusions.excluded(name, false))
            continue;
        // The working tree is read as a walk reads it: a link there (a symbolic link, a junction) may point anywhere
        // on the machine, and is not followed. A staged link is only the text of its target.
        if (!staged && is_link(fs::u8path(root) / fs::u8path(name))) continue;
        Source s;
        s.name = name;
        s.path = name;
        if (staged) {
            // The staged content, by its object id (`git show :<path>` would read `0:app.py` as stage 0 of `app.py`).
            if (change.id.find_first_not_of('0') == std::string::npos) {
                plan.failures.push_back({name, "could not be read: not in the index"});
                continue;
            }
            const std::string size_text = trim(git({"-C", root, "cat-file", "-s", change.id}));
            const uint64_t size = std::strtoull(size_text.c_str(), nullptr, 10);
            if (size == 0) continue;
            if (size > limit) {
                plan.failures.push_back({name, over_limit(size, limit)});
                continue;
            }
            s.size = static_cast<int64_t>(size);
            s.read = [root, id = change.id](std::vector<uint8_t>& out) {
                const ProcessResult r = run_process(git_command({"-C", root, "cat-file", "blob", id}));
                if (r.exit_code != 0) return false;
                out.assign(r.output.begin(), r.output.end());
                return true;
            };
        } else {
            const std::string disk = (fs::u8path(root) / fs::u8path(name)).u8string();
            std::error_code ec;
            const uintmax_t size = fs::file_size(fs::u8path(disk), ec);
            if (ec) {
                plan.failures.push_back({name, "could not be read: " + ec.message()});
                continue;
            }
            if (size == 0) continue;
            if (size > limit) {
                plan.failures.push_back({name, over_limit(size, limit)});
                continue;
            }
            s.size = static_cast<int64_t>(size);
            s.read = [disk](std::vector<uint8_t>& out) { return read_disk_file(disk, out); };
        }
        std::vector<std::string> patch = {
            "-C", root, "diff", "-U0", "--inter-hunk-context=0", "--no-color", "--no-ext-diff", "--no-textconv"};
        patch.insert(patch.end(), diff_args.begin(), diff_args.end());
        patch.push_back("--");
        if (!change.old_path.empty()) patch.push_back(change.old_path);
        patch.push_back(name);
        s.added = added_lines(git(patch));
        plan.sources.push_back(std::move(s));
    }
    return plan;
}

}  // namespace

bool AddedLines::contains(int64_t first, int64_t last) const {
    if (all) return true;
    for (const auto& r : ranges)
        if (first <= r.second && r.first <= last) return true;
    return false;
}

InputPlan plan_paths(const std::vector<std::string>& paths, const pb_detector* detector, bool skip_warnings,
                     const std::vector<std::string>& excludes) {
    InputPlan plan;
    const uint64_t limit = pb_detector_max_input_bytes(detector);
    const WalkPolicy policy{detector, skip_warnings};
    std::vector<Listed> files;
    for (const std::string& p : paths) {
        if (p == "-") {
            auto content = std::make_shared<std::vector<uint8_t>>(read_stdin());
            if (content->size() > limit) {
                plan.failures.push_back({"<stdin>", over_limit(content->size(), limit)});
                continue;
            }
            Source s;
            s.name = "<stdin>";
            s.size = static_cast<int64_t>(content->size());
            s.read = [content](std::vector<uint8_t>& out) {
                out = *content;
                return true;
            };
            plan.sources.push_back(std::move(s));
            continue;
        }
        const fs::path path = fs::u8path(p);
        const std::string shown = display_path(p);
        const bool outside = outside_project(path);
        std::error_code ec;
        if (fs::is_directory(path, ec)) {
            Exclusions exclusions;  // --exclude, then the .purebyteignore of the folder: paths from its top
            for (const std::string& e : excludes) exclusions.add(e);
            exclusions.load((path / ".purebyteignore").u8string());
            walk(path, shown, outside ? std::string() : shown, std::string(), policy, exclusions, files, plan.failures,
                 0);
        } else if (fs::exists(path, ec))  // a file named explicitly is always scanned
            files.push_back({shown, path, outside ? path.filename().u8string() : shown});
        else
            plan.failures.push_back({shown, "does not exist"});
    }
    std::sort(files.begin(), files.end());
    for (const Listed& f : files) {
        std::error_code ec;
        const uintmax_t size = fs::file_size(f.disk, ec);
        if (ec) {
            plan.failures.push_back({f.display, "could not be read: " + ec.message()});
            continue;
        }
        if (size == 0) continue;
        if (size > limit) {
            plan.failures.push_back({f.display, over_limit(size, limit)});
            continue;
        }
        const std::string disk = f.disk.u8string();
        Source s;
        s.name = f.display;
        s.path = f.rule;
        s.size = static_cast<int64_t>(size);
        s.read = [disk](std::vector<uint8_t>& out) { return read_disk_file(disk, out); };
        plan.sources.push_back(std::move(s));
    }
    return plan;
}

InputPlan plan_staged(const std::string& path, const pb_detector* detector, bool skip_warnings,
                      const std::vector<std::string>& excludes) {
    return plan_git({"--cached"}, path, true, WalkPolicy{detector, skip_warnings}, excludes);
}

InputPlan plan_git_diff(const std::string& ref, const std::string& path, const pb_detector* detector,
                        bool skip_warnings, const std::vector<std::string>& excludes) {
    if (ref.empty() || ref[0] == '-')
        throw UsageError("--git-diff takes a git reference such as origin/main, not `" + ref + "`");
    return plan_git({ref}, path, false, WalkPolicy{detector, skip_warnings}, excludes);
}

std::vector<uint8_t> read_one(const std::string& path, uint64_t limit, bool model_limit) {
    std::vector<uint8_t> bytes;
    const std::string shown = path == "-" ? "<stdin>" : display_path(path);
    if (path == "-") {
        bytes = read_stdin();
    } else {
        std::error_code ec;
        const uintmax_t size = fs::file_size(fs::u8path(path), ec);
        if (ec) throw UsageError("cannot read " + shown + ": " + ec.message(), 2);
        if (size > limit) throw UsageError(shown + ": " + over_limit(size, limit, model_limit), 2);
        if (!read_disk_file(path, bytes)) throw UsageError("cannot read " + shown, 2);
    }
    if (bytes.size() > limit) throw UsageError(shown + ": " + over_limit(bytes.size(), limit, model_limit), 2);
    return bytes;
}

}  // namespace cli
