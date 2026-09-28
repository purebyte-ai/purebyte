// A small decision: one input of at most 4 KB -> positive or negative, with the spans. Shared by `purebyte decide`
// and `POST /v1/decide`.
#pragma once

#include <cstddef>
#include <string>

#include "detection.h"
#include "setup.h"

namespace cli {

constexpr size_t kDecideLimit = 4096;

struct Decision {
    bool positive = false;
    std::string json;  // {"decision": "positive" | "negative", "model": ..., "result": {...}, "findings": [...]}
};

// Positive when the profile reports findings; for a model without a span head, when its first choice head's decision
// is not class 0.
Decision decide(const Setup& setup, const NamedInput& input);

}  // namespace cli
