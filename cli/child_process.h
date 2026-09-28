// Running another program (git, curl) without a shell: arguments are passed as they are, never interpreted.
#pragma once

#include <string>
#include <vector>

namespace cli {

struct ProcessResult {
    int exit_code = -1;  // -1 when the program could not be started (`errors` then says why)
    std::string output;  // standard output
    std::string errors;  // standard error
};

// Where `name` is found in the absolute folders of PATH, or "" when none has it. Empty, `.` and relative entries of
// PATH are skipped and the current folder is never searched (as os/exec does in Go since 1.19), so a `git` or `curl`
// that a scanned repository ships at its root is never run. On Windows, a name without an extension gets those of
// PATHEXT that name programs (.com and .exe; never .bat or .cmd, whose arguments cmd.exe would parse again). A name
// that holds a folder separator is returned as it is.
std::string find_program(const std::string& name);

// Runs `argv[0]` (looked up with find_program) with the given arguments and waits for it.
ProcessResult run_process(const std::vector<std::string>& argv);

}  // namespace cli
