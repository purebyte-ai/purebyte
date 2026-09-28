#include "detect/spans.h"

#include <algorithm>
#include <set>
#include <tuple>

namespace pb::detect {

std::vector<Candidate> merge_nearby(std::vector<Candidate> spans, int64_t gap) {
    std::vector<Candidate> out;
    if (spans.empty()) return out;
    std::sort(spans.begin(), spans.end(), [](const Candidate& x, const Candidate& y) {
        return x.start != y.start ? x.start < y.start : x.end < y.end;
    });
    out.push_back(spans[0]);
    for (size_t k = 1; k < spans.size(); ++k) {
        Candidate& last = out.back();
        if (spans[k].start <= last.end + gap) {
            last.end = std::max(last.end, spans[k].end);
            if (spans[k].confidence > last.confidence) {
                last.confidence = spans[k].confidence;
                last.type = spans[k].type;
            }
        } else {
            out.push_back(spans[k]);
        }
    }
    return out;
}

bool is_validated_type(const std::string& name) {
    static const std::set<std::string> kValidated = {"IBAN",  "CREDIT_CARD", "DNI_NIE",
                                                     "EMAIL", "IP",          "URL_CREDENTIALS"};
    return kValidated.count(name) > 0;
}

namespace {

std::string join_sources(const std::string& a, const std::string& b) {
    if (a == b) return a;
    std::set<std::string> parts;
    for (const std::string* s : {&a, &b}) {
        size_t start = 0;
        for (size_t plus; (plus = s->find('+', start)) != std::string::npos; start = plus + 1)
            parts.insert(s->substr(start, plus - start));
        parts.insert(s->substr(start));
    }
    std::string out;
    for (const std::string& p : parts) out += (out.empty() ? "" : "+") + p;
    return out;
}

}  // namespace

std::vector<Candidate> resolve(std::vector<Candidate> spans, const std::vector<std::string>& type_names) {
    spans.erase(std::remove_if(spans.begin(), spans.end(), [](const Candidate& s) { return s.end <= s.start; }),
                spans.end());
    std::stable_sort(spans.begin(), spans.end(), [](const Candidate& x, const Candidate& y) {
        return std::make_tuple(x.type, x.start, -x.end) < std::make_tuple(y.type, y.start, -y.end);
    });
    std::vector<Candidate> merged;
    for (const Candidate& s : spans) {
        if (!merged.empty() && merged.back().type == s.type && s.start < merged.back().end) {
            Candidate& m = merged.back();
            m.end = std::max(m.end, s.end);
            m.confidence = std::max(m.confidence, s.confidence);
            m.source = join_sources(m.source, s.source);
        } else {
            merged.push_back(s);
        }
    }
    if (merged.empty()) return merged;
    // Paint the ownership of every byte, best span first.
    auto validated = [&](const Candidate& c) {
        const bool known = c.type >= 0 && static_cast<size_t>(c.type) < type_names.size();
        return known && is_validated_type(type_names[c.type]) && c.source.find("rules") != std::string::npos;
    };
    std::vector<size_t> order(merged.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t i, size_t j) {
        const Candidate &x = merged[i], &y = merged[j];
        return std::make_tuple(validated(x), x.confidence, x.end - x.start) >
               std::make_tuple(validated(y), y.confidence, y.end - y.start);
    });
    int64_t lo = merged[0].start, hi = merged[0].end;
    for (const Candidate& c : merged) {
        lo = std::min(lo, c.start);
        hi = std::max(hi, c.end);
    }
    std::vector<int64_t> owner(static_cast<size_t>(hi - lo), -1);
    for (size_t i : order)
        for (int64_t p = merged[i].start; p < merged[i].end; ++p)
            if (owner[p - lo] < 0) owner[p - lo] = static_cast<int64_t>(i);
    std::vector<Candidate> out;
    for (int64_t p = 0; p < hi - lo;) {
        int64_t q = p + 1;
        while (q < hi - lo && owner[q] == owner[p]) ++q;
        if (owner[p] >= 0) {
            Candidate c = merged[owner[p]];
            c.start = p + lo;
            c.end = q + lo;
            out.push_back(c);
        }
        p = q;
    }
    return out;
}

}  // namespace pb::detect
