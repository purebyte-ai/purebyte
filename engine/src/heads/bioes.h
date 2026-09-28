// BIOES decoding for k entity types.
//
// Labels: 0 = O and, for entity e in [0, k): B = 1+4e, I = 2+4e, E = 3+4e, S = 4+4e (L = 1 + 4k labels).
// Allowed transitions: from O, E-x and S-x to O, B-y or S-y; from B-x and I-x to I-x or E-x (same entity). A sequence
// starts with O, B or S and ends with O, E or S. Viterbi runs in double precision with the per-label bias added to
// the log-probabilities; on equal scores the lower label index wins.
#pragma once

#include <cstdint>
#include <vector>

#include "heads/head.h"

namespace pb::bioes {

inline int labels_for(int entities) { return 1 + 4 * entities; }

// Most likely valid label sequence. logp [T, L] log-probabilities; bias [L] added to every position (may be null).
void viterbi(const float* logp, int T, int L, int entities, const float* bias, std::vector<int>& path);

// Spans (start, exclusive end, entity) of a valid label sequence, with the confidence of each: the mean over its bytes
// of the probability of the chosen label, exp(logp[t][path[t]]) without the bias.
void spans(const std::vector<int>& path, const float* logp, int L, std::vector<Span>& out);

}  // namespace pb::bioes
