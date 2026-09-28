// Runs a model over one input as a single sequence and returns its final hidden states.
#pragma once

#include <vector>

#include "model/model.h"
#include "runtime/scan.h"
#include "runtime/session.h"

namespace pb {

// hidden [size, d_model] after the final norm (and the lookahead, when the model has one). All the session's threads
// work on the one sequence.
std::vector<float> encode(Session& session, const Model& model, Bytes input);

}  // namespace pb
