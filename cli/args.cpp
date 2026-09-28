#include "args.h"

#include <cmath>
#include <cstdlib>

namespace cli {

Args::Args(const std::vector<std::string>& argv, size_t first, const std::vector<OptionSpec>& spec) {
    auto find = [&](const std::string& name) -> const OptionSpec* {
        for (const OptionSpec& o : spec)
            if (name == o.name) return &o;
        return nullptr;
    };
    bool options_ended = false;
    for (size_t i = first; i < argv.size(); ++i) {
        const std::string& a = argv[i] == "-h" && find("help") && !options_ended ? std::string("--help") : argv[i];
        if (options_ended || a.size() < 3 || a.compare(0, 2, "--") != 0) {
            if (a == "--")
                options_ended = true;
            else
                positional_.push_back(a);
            continue;
        }
        std::string name = a.substr(2), value;
        const size_t eq = name.find('=');
        const bool inline_value = eq != std::string::npos;
        if (inline_value) {
            value = name.substr(eq + 1);
            name = name.substr(0, eq);
        }
        const OptionSpec* o = find(name);
        if (!o) throw UsageError("unknown option `--" + name + "` (see --help)");
        if (o->value) {
            if (!inline_value) {
                if (i + 1 >= argv.size()) throw UsageError("`--" + name + "` needs a value (" + o->value + ")");
                value = argv[++i];
            }
        } else if (inline_value) {
            throw UsageError("`--" + name + "` takes no value");
        }
        if (values_.count(name) && !o->repeatable) throw UsageError("`--" + name + "` given twice");
        values_[name].push_back(value);
    }
}

std::string Args::text(const std::string& name, const std::string& fallback) const {
    const auto it = values_.find(name);
    return it == values_.end() ? fallback : it->second.back();
}

std::vector<std::string> Args::all(const std::string& name) const {
    const auto it = values_.find(name);
    return it == values_.end() ? std::vector<std::string>() : it->second;
}

int Args::integer(const std::string& name, int fallback, int lo, int hi) const {
    if (!has(name)) return fallback;
    const std::string v = text(name);
    char* end = nullptr;
    const long x = std::strtol(v.c_str(), &end, 10);
    if (v.empty() || *end || x < lo || x > hi)
        throw UsageError("`--" + name + "` takes an integer from " + std::to_string(lo) + " to " + std::to_string(hi) +
                         ", not `" + v + "`");
    return static_cast<int>(x);
}

double Args::real(const std::string& name, double fallback) const {
    if (!has(name)) return fallback;
    const std::string v = text(name);
    char* end = nullptr;
    const double x = std::strtod(v.c_str(), &end);
    if (v.empty() || *end || !std::isfinite(x)) throw UsageError("`--" + name + "` takes a number, not `" + v + "`");
    return x;
}

std::string options_help(const std::vector<OptionSpec>& spec) {
    std::string out;
    for (const OptionSpec& o : spec) {
        std::string left = "  --" + std::string(o.name) + (o.value ? " " + std::string(o.value) : "");
        if (left.size() < 26)
            left.resize(26, ' ');
        else
            left += "  ";
        out += left + o.help + "\n";
    }
    return out;
}

std::vector<std::string> split_list(const std::string& s, char separator) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t at; (at = s.find(separator, start)) != std::string::npos; start = at + 1)
        if (at > start) out.push_back(s.substr(start, at - start));
    if (start < s.size()) out.push_back(s.substr(start));
    return out;
}

}  // namespace cli
