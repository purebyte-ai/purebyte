#include "exclusions.h"

#include <algorithm>
#include <filesystem>

#include "args.h"
#include "catalog.h"

namespace cli {

namespace {

std::vector<std::string> segments(const std::string& path) {
    std::vector<std::string> out;
    std::string part;
    for (const char c : path + "/") {
        if (c != '/' && c != '\\') {
            part += c;
            continue;
        }
        if (!part.empty() && part != ".") out.push_back(part);
        part.clear();
    }
    return out;
}

// hits[n] says whether the pattern's segments match the path's first n segments exactly, where `**` takes any number
// of them. One pass of dynamic programming over (pattern segment, path segment): the work grows with the product of
// their counts, whatever the number of `**` (a search that tries every split grows exponentially with them, and a
// .purebyteignore comes from the repository being scanned).
std::vector<char> prefix_matches(const std::vector<std::string>& p, const std::vector<std::string>& s) {
    std::vector<char> row(s.size() + 1, 0), next(s.size() + 1, 0);
    row[0] = 1;  // no pattern segment matches no path segment
    for (const std::string& part : p) {
        std::fill(next.begin(), next.end(), 0);
        if (part == "**") {
            char reached = 0;
            for (size_t j = 0; j <= s.size(); ++j) next[j] = reached |= row[j];
        } else {
            for (size_t j = 0; j < s.size(); ++j)
                if (row[j] && glob_match(part, s[j])) next[j + 1] = 1;
        }
        row.swap(next);
    }
    return row;
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n"), z = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, z - a + 1);
}

}  // namespace

bool glob_match(const std::string& pattern, const std::string& name) {
    size_t p = 0, n = 0, star = std::string::npos, resume = 0;
    while (n < name.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == name[n])) {
            ++p;
            ++n;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            resume = n;
        } else if (star != std::string::npos) {
            p = star + 1;
            n = ++resume;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

void Exclusions::add(const std::string& raw) {
    std::string text = trim(raw);
    if (text.empty()) throw UsageError("an exclusion pattern cannot be empty");
    if (text[0] == '!') throw UsageError("`" + text + "`: negated exclusion patterns (!) are not supported");
    if (text.find('[') != std::string::npos)
        throw UsageError("`" + text + "`: character classes ([...]) are not supported in exclusion patterns");
    Pattern pattern;
    if (text.back() == '/' || text.back() == '\\') {
        pattern.directory_only = true;
        text.pop_back();
    }
    const bool leading = text[0] == '/' || text[0] == '\\';
    for (const std::string& part : segments(text))  // `a/**/**/b` is `a/**/b`
        if (part != "**" || pattern.parts.empty() || pattern.parts.back() != "**") pattern.parts.push_back(part);
    if (pattern.parts.empty()) throw UsageError("`" + raw + "`: an exclusion pattern must name something");
    pattern.anchored = leading || pattern.parts.size() > 1;
    pattern.fixed = static_cast<size_t>(std::count_if(pattern.parts.begin(), pattern.parts.end(),
                                                      [](const std::string& part) { return part != "**"; }));
    patterns_.push_back(std::move(pattern));
}

void Exclusions::load(const std::string& file) {
    // Read as every other file is (pb::read_file): a UTF-8 path, which the narrow streams of Windows cannot open
    // outside the ANSI code page. The file comes with the repository scanned: a link is not followed.
    const std::filesystem::path path = std::filesystem::u8path(file);
    std::error_code ec;
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(path, ec)))
        throw UsageError("the exclusion file " + display_path(file) + " is a symbolic link, which is not followed", 2);
    if (!std::filesystem::exists(path, ec)) return;
    const std::vector<uint8_t> bytes = read_whole_file(file, "the exclusion file");
    std::string text(bytes.begin(), bytes.end());
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);  // a UTF-8 byte order mark
    for (size_t start = 0; start < text.size();) {
        const size_t end = std::min(text.find('\n', start), text.size());
        const std::string t = trim(text.substr(start, end - start));
        start = end + 1;
        if (t.empty() || t[0] == '#') continue;
        add(t);
    }
}

bool Exclusions::excluded(const std::string& path, bool directory) const {
    const std::vector<std::string> s = segments(path);
    for (const Pattern& pattern : patterns_) {
        // The path itself, or one of the folders above it: a folder that matches leaves out everything below it.
        if (!pattern.anchored) {
            for (size_t length = 1; length <= s.size(); ++length) {
                const bool folder = length < s.size() || directory;
                if ((!pattern.directory_only || folder) && glob_match(pattern.parts[0], s[length - 1])) return true;
            }
            continue;
        }
        if (pattern.fixed > s.size()) continue;  // it names more folders than the path has
        const std::vector<char> hits = prefix_matches(pattern.parts, s);
        for (size_t length = 1; length <= s.size(); ++length) {
            const bool folder = length < s.size() || directory;
            if ((!pattern.directory_only || folder) && hits[length]) return true;
        }
    }
    return false;
}

}  // namespace cli
