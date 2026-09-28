// What `purebyte scan` reads: files and directories, standard input, the staged changes or a diff of a git repository.
// Inputs are listed first and read batch by batch, so memory stays bounded. Which files a directory walk selects and
// how large an input may be are the detector's profile's choice (pb_detector_wants_file & co.).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "purebyte/pb.h"

namespace cli {

// Line ranges (1-based, inclusive) that a diff adds to a file; `all` = every line counts.
struct AddedLines {
    bool all = true;
    std::vector<std::pair<int64_t, int64_t>> ranges;
    bool contains(int64_t first, int64_t last) const;
};

struct Source {
    std::string name;                                 // display name
    std::string path;                                 // its path inside the project scanned (pb_detect_options.paths)
    int64_t size = 0;                                 // bytes, when known before reading
    std::function<bool(std::vector<uint8_t>&)> read;  // loads the content; false when it cannot
    AddedLines added;                                 // --staged / --git-diff: where findings are kept
};

struct Failure {
    std::string file, reason;
};

struct InputPlan {
    std::vector<Source> sources;
    std::vector<Failure> failures;
};

// Files and directories named on the command line ("-" is standard input). Directories are walked recursively;
// files named explicitly are always selected. Empty files are skipped; files over the profile's limit are failures.
//
// Each source's `path`, which the profile's path rules judge (pb_detector_path_severity), is where the file sits in
// the project scanned: a relative argument is taken as it is written (`purebyte scan .` from the repository root);
// below an absolute argument or one that climbs with `..`, only the part inside it counts, so that the folders above
// a project (/home/me/test/repo) never turn its findings into warnings; a file named that way counts by its name.
// `skip_warnings`: leave out, while walking, the files whose findings would be warnings (--tests=no). `excludes`:
// patterns of paths to leave out (--exclude, cli/exclusions.h), matched from the top of each folder named, together
// with the .purebyteignore file of that folder; files named explicitly are always scanned.
InputPlan plan_paths(const std::vector<std::string>& paths, const pb_detector* detector, bool skip_warnings = false,
                     const std::vector<std::string>& excludes = {});

// The files the index would commit (`--staged`) in the repository at `path`, with the lines the commit adds: files
// added, copied, modified, renamed (a file only moved adds no line) or whose type changed; submodules are not entered.
// Paths are relative to the repository's root, for the display, the path rules and the exclusions alike (`excludes`
// and the .purebyteignore file of the repository). As in a walk, empty files are skipped and files over the profile's
// limit are failures, found before they are read.
InputPlan plan_staged(const std::string& path, const pb_detector* detector, bool skip_warnings = false,
                      const std::vector<std::string>& excludes = {});
// The files changed between `ref` and the working tree under `path` (`--git-diff REF`), with the lines added.
InputPlan plan_git_diff(const std::string& ref, const std::string& path, const pb_detector* detector,
                        bool skip_warnings = false, const std::vector<std::string>& excludes = {});

// One input read from a file or standard input ("-"), for the commands that take a single input. `model_limit` says
// whose limit `limit` is in the message of an input over it: the model's, or the command's own.
std::vector<uint8_t> read_one(const std::string& path, uint64_t limit, bool model_limit = true);

}  // namespace cli
