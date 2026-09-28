// The options every model-running command shares (--model, --bias, --threads...) and what they set up.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "args.h"
#include "detection.h"
#include "engine.h"

namespace cli {

// --model, --ensemble, --no-ensemble, --profile, --bias, --type-bias, --votes, --min-confidence, --threads,
// --intra-threads, --kernel.
std::vector<OptionSpec> model_options();

struct Setup {
    Specialist specialist;
    std::shared_ptr<pb_session> session;
    std::shared_ptr<pb_detector> detector;
    std::string profile;           // the profile in use
    ModelDeclaration declaration;  // of the main model
    DetectSettings settings;       // bias, votes, min confidence, ensemble use
    int threads = 1;
};

// Loads the model named by --model (or `default_model`) and makes its detector, on `session` when given, else on a
// new session sized by --threads.
Setup setup(const Args& args, const std::string& default_model, std::shared_ptr<pb_session> session = nullptr);

}  // namespace cli
