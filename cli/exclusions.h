// Paths the user leaves out of a scan: `--exclude GLOB` (repeatable) and the lines of a `.purebyteignore` file at the
// top of a scanned folder (or of the repository, with --staged and --git-diff). The patterns follow .gitignore:
//
//   - a pattern without '/' matches a file or folder name at any depth: `*.min.js`, `node_modules`, `testdata`;
//   - a pattern with '/' matches the path from the top of the scan: `docs/examples/*`, `/build`;
//   - `**` stands for any number of folders: `**/fixtures/**`; `*` and `?` never cross a '/';
//   - a trailing '/' matches folders only: `vendor/`;
//   - a folder that matches leaves out everything below it;
//   - in the file, empty lines and lines that start with '#' are ignored.
//
// Negation (`!pattern`) and character classes (`[abc]`) are not supported, and are refused rather than guessed.
#pragma once

#include <string>
#include <vector>

namespace cli {

class Exclusions {
public:
    // Throws UsageError for a pattern that is empty, starts with '!' or holds a character class.
    void add(const std::string& pattern);
    // The patterns of a `.purebyteignore` file (a UTF-8 path); a missing file adds nothing, one that exists but cannot
    // be read is an error (UsageError, exit 2).
    void load(const std::string& file);
    // `path`: relative to the top of the scan, '/' or '\' separated; `directory`: whether it names a folder.
    bool excluded(const std::string& path, bool directory) const;
    bool empty() const { return patterns_.empty(); }

private:
    struct Pattern {
        std::vector<std::string> parts;  // the pattern's segments (no two `**` in a row); one for a name pattern
        size_t fixed = 0;                // segments other than `**`: a path with fewer cannot match
        bool anchored = false;           // matched from the top of the scan, not at any depth
        bool directory_only = false;     // a trailing '/'
    };
    std::vector<Pattern> patterns_;
};

// `*` and `?` glob of one path segment against one name.
bool glob_match(const std::string& pattern, const std::string& name);

}  // namespace cli
