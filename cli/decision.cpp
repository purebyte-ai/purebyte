#include "decision.h"

namespace cli {

Decision decide(const Setup& s, const NamedInput& input) {
    DetectSettings settings = s.settings;
    settings.expand_archives = false;  // one input, one answer
    const pb::json::Value out = detect(s.session.get(), s.detector.get(), {input}, settings);
    const pb::json::Value& result = out.get("results")->array.at(0);
    const pb::json::Value& findings = *out.get("findings");
    Decision d;
    if (!findings.array.empty()) {
        d.positive = true;
    } else if (s.declaration.entities.empty()) {
        const pb::json::Value* decisions = result.get("decisions");
        if (decisions && !decisions->object.empty())
            d.positive = number_of(decisions->object.front().second, "index") != 0;
    }
    pb::json::Writer w;
    w.begin_object().field("decision", d.positive ? "positive" : "negative");
    w.key("model")
        .begin_object()
        .field("name", s.specialist.name)
        .field("version", s.specialist.version)
        .field("profile", s.profile)
        .end_object();
    w.key("result").raw(pb::json::dump(result)).key("findings").raw(pb::json::dump(findings)).end_object();
    d.json = w.take();
    return d;
}

}  // namespace cli
