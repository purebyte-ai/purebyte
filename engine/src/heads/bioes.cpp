#include "heads/bioes.h"

#include <cmath>

namespace pb::bioes {

namespace {

constexpr double kForbidden = -1e30;

inline bool can_start(int label) { return label == 0 || (label - 1) % 4 == 0 || (label - 1) % 4 == 3; }  // O, B, S
inline bool can_end(int label) { return label == 0 || (label - 1) % 4 == 2 || (label - 1) % 4 == 3; }    // O, E, S

// The best predecessor among candidates: the highest score above -1e300 (NaN never is), the lowest label on ties;
// label 0 with -1e300 when there is none. It is what scanning every label in order and keeping a strictly higher
// score gives.
struct Best {
    double score = -1e300;
    int label = 0;
    void offer(double v, int j) {
        if (v > score || (v == score && v > -1e300 && j < label)) {
            score = v;
            label = j;
        }
    }
};

}  // namespace

// A step of the lattice costs O(L), not O(L^2): the transition into a label depends on its kind only.
//   * O, B-y and S-y (they can start) are entered from O, E-x and S-x (they can end) freely (+0) and from any B-x or
//     I-x with the penalty: one best predecessor serves all of them.
//   * I-y and E-y are entered from B-y and I-y freely and from every other label with the penalty: the best of the
//     two free ones against the best penalized label other than those two, found among the three best penalized
//     labels of the step.
// Every candidate score is the sum the full matrix would give (dp[j] + 0.0, or dp[j] + kForbidden), and ties go to
// the lowest label, so the path is the one of the O(L^2) search.
void viterbi(const float* logp, int T, int L, int entities, const float* bias, std::vector<int>& path) {
    std::vector<double> dp(static_cast<size_t>(L)), next(static_cast<size_t>(L));
    std::vector<int32_t> back(static_cast<size_t>(T) * L, 0);
    for (int k = 0; k < L; ++k)
        dp[k] = static_cast<double>(logp[k]) + (bias ? bias[k] : 0.0) + (can_start(k) ? 0.0 : kForbidden);
    for (int t = 1; t < T; ++t) {
        Best starting;  // into O, B-y, S-y
        // The three best penalized scores dp[j] + kForbidden above -1e300, best first (labels come in increasing
        // order, so an equal score stays behind the lower label).
        double penalized[3] = {0.0, 0.0, 0.0};
        int penalized_label[3] = {0, 0, 0}, count = 0;
        for (int j = 0; j < L; ++j) {
            starting.offer(dp[j] + (can_end(j) ? 0.0 : kForbidden), j);
            const double v = dp[j] + kForbidden;
            if (!(v > -1e300)) continue;
            int at = count;
            while (at > 0 && v > penalized[at - 1]) --at;
            if (at == 3) continue;
            for (int q = count < 3 ? count : 2; q > at; --q) {
                penalized[q] = penalized[q - 1];
                penalized_label[q] = penalized_label[q - 1];
            }
            penalized[at] = v;
            penalized_label[at] = j;
            if (count < 3) ++count;
        }
        auto step = [&](int k, const Best& best) {
            back[static_cast<size_t>(t) * L + k] = best.label;
            next[k] = best.score + static_cast<double>(logp[static_cast<size_t>(t) * L + k]) + (bias ? bias[k] : 0.0);
        };
        for (int k = 0; k < L; ++k)
            if (can_start(k)) step(k, starting);
        for (int e = 0; e < entities; ++e) {
            const int B = 1 + 4 * e, I = 2 + 4 * e, E = 3 + 4 * e;
            Best inside;  // into I-e and E-e
            inside.offer(dp[B] + 0.0, B);
            inside.offer(dp[I] + 0.0, I);
            for (int q = 0; q < count; ++q)
                if (penalized_label[q] != B && penalized_label[q] != I) {
                    inside.offer(penalized[q], penalized_label[q]);
                    break;
                }
            step(I, inside);
            step(E, inside);
        }
        dp.swap(next);
    }
    int last = 0;
    double best = -1e300;
    for (int k = 0; k < L; ++k) {
        const double v = dp[k] + (can_end(k) ? 0.0 : kForbidden);
        if (v > best) {
            best = v;
            last = k;
        }
    }
    path.assign(static_cast<size_t>(T), 0);
    path[T - 1] = last;
    for (int t = T - 1; t > 0; --t) path[t - 1] = back[static_cast<size_t>(t) * L + path[t]];
}

void spans(const std::vector<int>& path, const float* logp, int L, std::vector<Span>& out) {
    out.clear();
    auto confidence = [&](int a, int z) {
        double sum = 0.0;
        for (int t = a; t < z; ++t) sum += std::exp(static_cast<double>(logp[static_cast<size_t>(t) * L + path[t]]));
        return static_cast<float>(sum / (z - a));
    };
    int start = -1, entity = -1;
    for (int t = 0; t < static_cast<int>(path.size()); ++t) {
        const int l = path[t];
        if (l == 0) {
            start = -1;
            continue;
        }
        const int e = (l - 1) / 4, kind = (l - 1) % 4;  // 0 B, 1 I, 2 E, 3 S
        if (kind == 0) {
            start = t;
            entity = e;
        } else if (kind == 3) {
            out.push_back({t, t + 1, e, confidence(t, t + 1)});
            start = -1;
        } else if (kind == 2 && start >= 0 && entity == e) {
            out.push_back({start, t + 1, e, confidence(start, t + 1)});
            start = -1;
        }
    }
}

}  // namespace pb::bioes
