#include "detect/code_files.h"

#include <set>
#include <vector>

namespace pb::detect {

namespace {

// ASCII lowercase, plus U+212A KELVIN SIGN (the one non-ASCII letter whose lowercase is ASCII).
std::string lower_name(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i + 2 < s.size() && static_cast<unsigned char>(s[i]) == 0xE2 &&
            static_cast<unsigned char>(s[i + 1]) == 0x84 && static_cast<unsigned char>(s[i + 2]) == 0xAA) {
            out += 'k';
            i += 2;
            continue;
        }
        out += (s[i] >= 'A' && s[i] <= 'Z') ? static_cast<char>(s[i] + 32) : s[i];
    }
    return out;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

bool is_source_text_name(const std::string& file_name) {
    static const std::set<std::string> kExtensions = {
        "py",   "js",    "ts",         "jsx",  "tsx",  "java",   "go",         "rb",    "php",     "cs",
        "rs",   "yaml",  "yml",        "json", "toml", "env",    "sh",         "bash",  "zsh",     "cfg",
        "ini",  "conf",  "properties", "xml",  "tf",   "tfvars", "c",          "cpp",   "h",       "hpp",
        "kt",   "swift", "scala",      "pl",   "lua",  "sql",    "dockerfile", "pem",   "key",     "txt",
        "md",   "rst",   "adoc",       "ps1",  "psm1", "gradle", "groovy",     "kts",   "ex",      "exs",
        "dart", "vue",   "html",       "htm",  "csv",  "erb",    "tpl",        "j2",    "plist",   "cnf",
        "r",    "m",     "mjs",        "cjs",  "cc",   "hh",     "tfstate",    "ipynb", "htpasswd"};
    static const std::set<std::string> kNames = {
        "dockerfile", "jenkinsfile",      "makefile",    "procfile", "vagrantfile", "gemfile", "rakefile", "brewfile",
        "justfile",   "caddyfile",        ".env",        ".envrc",   ".netrc",      ".pgpass", ".npmrc",   ".pypirc",
        ".dockercfg", ".git-credentials", "credentials", "config"};
    const std::string low = lower_name(file_name);
    const size_t dot = low.rfind('.');
    const std::string extension = dot == std::string::npos ? low : low.substr(dot + 1);
    return kExtensions.count(extension) || kNames.count(low) || low.rfind("dockerfile", 0) == 0 ||
           low.rfind("jenkinsfile", 0) == 0;
}

bool is_dependency_directory(const std::string& directory_name) {
    static const std::set<std::string> kSkipped = {".git", "node_modules", "__pycache__", ".venv",  "venv",
                                                   "dist", "build",        ".next",       "target", "vendor"};
    return kSkipped.count(directory_name) > 0;
}

bool is_test_example_or_doc_path(const std::string& path) {
    static const std::set<std::string> kParts = {
        "test",     "tests",   "spec",        "specs",     "__tests__", "testdata", "test-data", "fixture",
        "fixtures", "jvmtest", "androidtest", "example",   "examples",  "sample",   "samples",   "doc",
        "docs",     "mock",    "mocks",       "__mocks__", "e2e",       "cypress"};
    static const std::set<std::string> kDocuments = {"md", "markdown", "mdx", "rst", "adoc", "asciidoc"};
    static const std::set<std::string> kCamelCaseTests = {"cs", "kt", "java", "scala", "swift"};
    std::string name;  // the last part: the file name
    for (size_t start = 0, end = 0; start <= path.size(); start = end + 1) {
        end = path.find_first_of("/\\", start);
        if (end == std::string::npos) end = path.size();
        name = path.substr(start, end - start);
        if (kParts.count(lower_name(name))) return true;
    }
    const std::string low = lower_name(name);
    const size_t dot = low.rfind('.');
    if (dot == std::string::npos || dot == 0) return false;
    const std::string stem = low.substr(0, dot), extension = low.substr(dot + 1);
    if (kDocuments.count(extension)) return true;
    if (extension == "py" && low.compare(0, 5, "test_") == 0) return true;  // test_api.py
    // app.test.js, app.spec.ts, user_spec.rb, handler_test.go, util-tests.js: `.`, `_` or `-`, then test(s) or
    // spec(s), then one extension of letters and digits.
    static const std::vector<std::string> kTestWords = {"test", "tests", "spec", "specs"};
    bool alphanumeric = !extension.empty();
    for (const char c : extension) alphanumeric = alphanumeric && ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'));
    for (const std::string& word : kTestWords) {
        if (!alphanumeric || stem.size() <= word.size()) continue;
        const char before = stem[stem.size() - word.size() - 1];
        if (ends_with(stem, word) && (before == '.' || before == '_' || before == '-')) return true;
    }
    // UserTest.java, UserTests.cs: "Test" with a capital T, so that Latest.java is not a test; or a file named test.
    if (!kCamelCaseTests.count(extension)) return false;
    const std::string original = name.substr(0, name.rfind('.'));
    return ends_with(original, "Test") || ends_with(original, "Tests") || stem == "test" || stem == "tests";
}

}  // namespace pb::detect
