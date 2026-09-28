// The head types this library implements. Adding one: a new module with its loader, and one line here.
#include "heads/head.h"

namespace pb {

namespace {

const HeadType kHeadTypes[] = {
    {"choice", load_choice_head}, {"multilabel", load_multilabel_head},
    {"score", load_score_head},   {"ordinal", load_ordinal_head},
    {"tag", load_tag_head},       {"byte_map", load_byte_map_head},
    {"exit", load_exit_head},
};

}  // namespace

const HeadType* find_head_type(const std::string& name) {
    for (const HeadType& t : kHeadTypes)
        if (name == t.name) return &t;
    return nullptr;
}

std::string known_head_types() {
    std::string names;
    for (const HeadType& t : kHeadTypes) names += (names.empty() ? "" : ", ") + std::string(t.name);
    return names;
}

}  // namespace pb
