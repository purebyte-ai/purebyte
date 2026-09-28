// The block types this library implements. Adding one: a new module with its loader, and one line here.
#include "blocks/block.h"

namespace pb {

namespace {

const BlockType kBlockTypes[] = {
    {"ssm_v2", load_ssm_block},
    {"attention", load_attention_block},
};

}  // namespace

const BlockType* find_block_type(const std::string& name) {
    for (const BlockType& t : kBlockTypes)
        if (name == t.name) return &t;
    return nullptr;
}

std::string known_block_types() {
    std::string names;
    for (const BlockType& t : kBlockTypes) names += (names.empty() ? "" : ", ") + std::string(t.name);
    return names;
}

}  // namespace pb
