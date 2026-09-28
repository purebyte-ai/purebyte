// The subcommands of `purebyte`. Each takes the whole argv (argv[1] is the command) and returns the exit status.
#pragma once

#include <string>
#include <vector>

namespace cli {

int command_scan(const std::vector<std::string>& argv);
int command_decide(const std::vector<std::string>& argv);
int command_redact(const std::vector<std::string>& argv);
int command_serve(const std::vector<std::string>& argv);
int command_models(const std::vector<std::string>& argv);
int command_bench(const std::vector<std::string>& argv);
int command_info(const std::vector<std::string>& argv);
int command_version(const std::vector<std::string>& argv);

}  // namespace cli
