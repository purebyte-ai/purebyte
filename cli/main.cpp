// purebyte: one program, one command per job (docs/cli.md). The commands use the library through its C API.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "args.h"
#include "commands.h"

#ifdef _WIN32
#include <windows.h>  // before shellapi.h, which needs its types

#include <shellapi.h>
#endif

namespace {

const char* kUsage =
    "usage: purebyte <command> [options]\n"
    "\n"
    "  scan      scan files, directories, standard input, git changes or archives, and report findings\n"
    "  decide    answer for one small input (up to 4 KB): the decision and the spans\n"
    "  redact    write a copy of the input with every detected span replaced by a typed marker\n"
    "  serve     run the local HTTP API\n"
    "  models    list, download and verify specialists\n"
    "  bench     measure latency and throughput on this machine\n"
    "  info      describe the runtime, or what a model file declares\n"
    "  version   print the version\n"
    "\n"
    "`purebyte <command> --help` lists the options of a command.\n";

// The arguments as UTF-8 on every platform (Windows passes them in UTF-16).
std::vector<std::string> utf8_arguments(int argc, char** argv) {
    std::vector<std::string> out;
#ifdef _WIN32
    int n = 0;
    if (LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &n)) {
        for (int i = 0; i < n; ++i) {
            const int len = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
            std::string s(static_cast<size_t>(len > 1 ? len - 1 : 0), '\0');
            if (len > 1) WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, &s[0], len, nullptr, nullptr);
            out.push_back(s);
        }
        LocalFree(wide);
        return out;
    }
#endif
    for (int i = 0; i < argc; ++i) out.emplace_back(argv[i]);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args = utf8_arguments(argc, argv);
    if (args.size() < 2 || args[1] == "--help" || args[1] == "-h" || args[1] == "help") {
        std::fputs(kUsage, args.size() < 2 ? stderr : stdout);
        return args.size() < 2 ? 2 : 0;
    }
    const std::string& command = args[1];
    try {
        if (command == "scan") return cli::command_scan(args);
        if (command == "decide") return cli::command_decide(args);
        if (command == "redact") return cli::command_redact(args);
        if (command == "serve") return cli::command_serve(args);
        if (command == "models") return cli::command_models(args);
        if (command == "bench") return cli::command_bench(args);
        if (command == "info") return cli::command_info(args);
        if (command == "version" || command == "--version") return cli::command_version(args);
        std::fprintf(stderr, "purebyte: unknown command `%s`\n\n%s", command.c_str(), kUsage);
        return 2;
    } catch (const cli::UsageError& e) {
        std::fprintf(stderr, "purebyte: %s\n", e.what());
        return e.exit_code();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "purebyte: %s\n", e.what());
        return 2;
    }
}
