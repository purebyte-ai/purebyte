#include "setup.h"

namespace cli {

std::vector<OptionSpec> model_options() {
    return {
        {"model", "NAME", "installed specialist (NAME or NAME@VERSION) or model file (.gguf)"},
        {"ensemble", "FILE", "add a model file to the ensemble (repeatable)", true},
        {"no-ensemble", nullptr, "use only the first model of an ensemble"},
        {"profile", "NAME", "post-processing profile (default: the model's); `none` for raw outputs"},
        {"bias", "X", "operating point of span heads: lower is more precise, higher finds more"},
        {"type-bias", "T=X,...", "operating point per entity type"},
        {"votes", "K", "ensemble votes a finding needs (default: a majority)"},
        {"min-confidence", "X", "drop spans below this confidence"},
        {"threads", "N", "worker threads (default: hardware threads minus one)"},
        {"intra-threads", "N", "threads that share one window (default: automatic)"},
        {"kernel", "NAME", "CPU kernel: auto, scalar, avx2, neon (default auto)"},
    };
}

Setup setup(const Args& a, const std::string& default_model, std::shared_ptr<pb_session> session) {
    if (a.has("ensemble") && a.has("no-ensemble"))
        throw UsageError("--ensemble and --no-ensemble are alternatives: give one");
    Setup s;
    s.specialist = load_specialist(a.text("model", default_model), a.all("ensemble"), !a.has("no-ensemble"));
    s.threads = a.integer("threads", default_threads(), 1, 1024);
    s.session =
        session ? session : make_session(s.threads, a.integer("intra-threads", 0, 0, 1024), a.text("kernel", "auto"));
    s.detector = make_detector(s.specialist, a.text("profile"));
    s.profile = pb_detector_profile(s.detector.get());
    s.declaration = declaration_of(s.specialist.members[0].handle.get());
    s.settings.no_ensemble = a.has("no-ensemble");
    s.settings.votes = a.integer("votes", 0, 0, 1024);
    const int members = static_cast<int>(s.specialist.members.size());
    if (s.settings.votes > members)
        throw UsageError("--votes " + std::to_string(s.settings.votes) +
                         " needs at least that many models; the ensemble has " + std::to_string(members));
    s.settings.has_bias = a.has("bias");
    s.settings.bias = static_cast<float>(a.real("bias", 0.0));
    if (a.has("type-bias")) {
        if (s.declaration.entities.empty()) throw UsageError("--type-bias: the model has no span head");
        ModelDeclaration base = s.declaration;
        if (s.settings.has_bias) base.type_bias.assign(base.entities.size(), s.settings.bias);
        s.settings.type_bias = parse_type_bias(a.text("type-bias"), base);
    }
    s.settings.min_confidence = static_cast<float>(a.real("min-confidence", 0.0));
    return s;
}

}  // namespace cli
