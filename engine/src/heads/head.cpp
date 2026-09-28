#include "heads/head.h"

#include "core/failure.h"

namespace pb {

Head::Head(LoadContext& ctx, int index, const char* type) : file_(ctx.file()), index_(index) {
    name_ = file_.text(LoadContext::key("head", index, "name"), type);
    labels_ = file_.texts(LoadContext::key("head", index, "labels"));
    // -1 (ungated) or the index of a head (at most 64 of them; the model checks the one named exists and can gate).
    gated_by_ = static_cast<int>(checked_int(file_, LoadContext::key("head", index, "gated_by"), -1, 63, -1));
}

void Head::set_aggregate(const std::string& allowed, const char* fallback) {
    const std::string key = LoadContext::key("head", index_, "aggregate");
    aggregate_ = file_.text(key, fallback);
    if (("," + allowed + ",").find("," + aggregate_ + ",") == std::string::npos)
        fail(PB_ERR_FORMAT, format("`%s` = `%s`; a %s head aggregates windows by %s", key.c_str(), aggregate_.c_str(),
                                   type(), allowed.c_str()));
}

json::Writer Head::describe_begin() const {
    json::Writer w;
    w.begin_object().field("index", index_).field("type", type()).field("name", name_);
    w.key("labels").begin_array();
    for (const std::string& l : labels_) w.string(l);
    w.end_array();
    if (gated_by_ >= 0) w.field("gated_by", gated_by_);
    if (!aggregate_.empty()) w.field("aggregate", aggregate_);
    return w;
}

}  // namespace pb
