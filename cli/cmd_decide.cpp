// purebyte decide: one small input (up to 4 KB) -> the decision and the spans, as JSON. Exit 1 when positive.
#include <cstdio>

#include "catalog.h"
#include "commands.h"
#include "decision.h"
#include "inputs.h"
#include "output.h"
#include "setup.h"

namespace cli {

int command_decide(const std::vector<std::string>& argv) {
    std::vector<OptionSpec> spec = model_options();
    spec.insert(spec.end(),
                {
                    {"text", "TEXT", "decide on this text instead of a file"},
                    {"reveal", nullptr, "print detected values instead of masks"},
                    {"all-bytes", nullptr, "binaries: also report findings that touch no printable string"},
                    {"query", "FIELD", "a field to extract, for query-conditioned models (repeatable)", true},
                    {"help", nullptr, "show this help"},
                });
    const Args a(argv, 2, spec);
    if (a.has("help")) {
        std::fputs(
            ("usage: purebyte decide [options] [FILE | -]\n\nOne small input (up to 4096 bytes): the decision and "
             "the spans. Exit status 0 negative, 1 positive, 2 error.\n\n" +
             options_help(spec))
                .c_str(),
            stdout);
        return 0;
    }
    if (a.positional().size() > 1 || (a.has("text") && !a.positional().empty()))
        throw UsageError("decide takes one input");
    NamedInput input;
    if (a.has("text")) {
        const std::string t = a.text("text");
        input = {"<text>", std::vector<uint8_t>(t.begin(), t.end())};
    } else {
        const std::string source = a.positional().empty() ? "-" : a.positional()[0];
        input = {source == "-" ? "<stdin>" : display_path(source), read_one(source, kDecideLimit, false)};
    }
    if (input.bytes.size() > kDecideLimit)
        throw UsageError("decide takes at most " + std::to_string(kDecideLimit) + " bytes (got " +
                             std::to_string(input.bytes.size()) + "): use `purebyte scan`",
                         2);
    Setup s = setup(a, "secrets-code");
    s.settings.reveal = a.has("reveal");
    s.settings.strings_only = !a.has("all-bytes");
    s.settings.queries = a.all("query");
    const Decision d = decide(s, input);
    Output out{std::string()};
    out.write(d.json);
    out.close();
    return d.positive ? 1 : 0;
}

}  // namespace cli
