// Command-line parsing: every command declares its options; anything else is an error, never silently ignored.
#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace cli {

// An error the user can fix; main() prints it and exits with `exit_code`.
class UsageError : public std::runtime_error {
public:
    explicit UsageError(const std::string& message, int exit_code = 2)
        : std::runtime_error(message), exit_code_(exit_code) {}
    int exit_code() const { return exit_code_; }

private:
    int exit_code_;
};

struct OptionSpec {
    const char* name;   // without the leading "--"
    const char* value;  // placeholder of its value ("N", "FILE"...), or nullptr for a flag
    const char* help;
    bool repeatable = false;
};

class Args {
public:
    // Parses argv[first..]: `--name value`, `--name=value`, flags, `-` (standard input) and positional arguments;
    // `--` ends the options. Throws UsageError on an unknown option or a missing value.
    Args(const std::vector<std::string>& argv, size_t first, const std::vector<OptionSpec>& spec);

    const std::vector<std::string>& positional() const { return positional_; }
    bool has(const std::string& name) const { return values_.count(name) > 0; }
    std::string text(const std::string& name, const std::string& fallback = "") const;
    std::vector<std::string> all(const std::string& name) const;
    int integer(const std::string& name, int fallback, int lo, int hi) const;
    double real(const std::string& name, double fallback) const;

private:
    std::vector<std::string> positional_;
    std::map<std::string, std::vector<std::string>> values_;
};

// The option table as help text.
std::string options_help(const std::vector<OptionSpec>& spec);

// "a,b,c" -> {"a", "b", "c"} (empty items dropped).
std::vector<std::string> split_list(const std::string& s, char separator = ',');

}  // namespace cli
