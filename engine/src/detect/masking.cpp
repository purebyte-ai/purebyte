#include "detect/masking.h"

#include <algorithm>
#include <iterator>
#include <limits>

#include "detect/text.h"

namespace pb::detect {

namespace {

struct Replacement {
    int64_t start, end;
    std::string text;
};

// Bytes [lo, hi) as text, with the replacements applied. Overlapping replacements are merged: the text of the first
// one stands for their union, so that no byte of any of them is shown.
std::string splice(const std::vector<uint8_t>& b, int64_t lo, int64_t hi, std::vector<Replacement> replacements,
                   bool ascii) {
    std::stable_sort(replacements.begin(), replacements.end(),
                     [](const Replacement& x, const Replacement& y) { return x.start < y.start; });
    auto decode = [&](int64_t a, int64_t z) {
        if (z <= a) return std::string();
        return ascii ? ascii_text(b.data() + a, static_cast<size_t>(z - a)) : text_of(b, a, z);
    };
    std::string out;
    int64_t pos = lo;
    for (const Replacement& r : replacements) {
        if (r.start < pos) {
            pos = std::max(pos, r.end);
            continue;
        }
        out += decode(pos, r.start);
        out += r.text;
        pos = r.end;
    }
    return out + decode(pos, hi);
}

// At most `limit` code points, with an ellipsis when cut.
std::string cut(const std::string& s, size_t limit) {
    size_t points = 0, i = 0;
    for (; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            if (points == limit) break;
            ++points;
        }
    }
    return i < s.size() ? s.substr(0, i) + "\xE2\x80\xA6" : s;
}

std::string blob_marker(int64_t run_bytes) { return "[base64 blob, " + std::to_string(run_bytes) + " B]"; }

// Characters of a credential-like token: letters, digits and + / = _ - . ~ (base64, base64url, hex, JWT).
bool is_token_byte(uint8_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' ||
           c == '=' || c == '_' || c == '-' || c == '.' || c == '~';
}

// [a, z) widened to the whole token around it, never past [lo, hi): a span that covers only part of a token (the
// first segment of a JWT, say) must not leave the rest of it in clear next to the mask. '=' only continues a token to
// the right (base64 padding), so that the name in `key=value` stays readable.
Range widen_to_token(const std::vector<uint8_t>& b, int64_t a, int64_t z, int64_t lo, int64_t hi) {
    while (a > lo && is_token_byte(b[a - 1]) && b[a - 1] != '=') --a;
    while (z < hi && is_token_byte(b[z])) ++z;
    return {a, z};
}

// A PEM header line that names a key type and nothing else: "-----BEGIN <words>-----" where every word is one of the
// key-type words of RFC 7468, OpenSSH and OpenPGP labels ("RSA PRIVATE KEY", "OPENSSH PRIVATE KEY", "PGP PUBLIC KEY
// BLOCK"...) and the label ends in KEY or KEY BLOCK. Any other word (a value written into the label) makes the line
// an ordinary value, masked like any other.
bool is_pem_key_header(const std::string& line) {
    static const char* const kWords[] = {"RSA",     "DSA",  "EC",  "ECDSA",     "ED25519", "ED448",  "X25519", "X448",
                                         "OPENSSH", "SSH2", "PGP", "ENCRYPTED", "PRIVATE", "PUBLIC", "KEY",    "BLOCK"};
    const std::string head = "-----BEGIN ", tail = "-----";
    if (line.size() <= head.size() + tail.size() || line.compare(0, head.size(), head) != 0 ||
        line.compare(line.size() - tail.size(), tail.size(), tail) != 0)
        return false;
    std::vector<std::string> words;
    const std::string label = line.substr(head.size(), line.size() - head.size() - tail.size());
    for (size_t at = 0;;) {
        const size_t space = label.find(' ', at);
        words.push_back(label.substr(at, space == std::string::npos ? std::string::npos : space - at));
        if (space == std::string::npos) break;
        at = space + 1;
    }
    for (const std::string& w : words)
        if (std::find(std::begin(kWords), std::end(kWords), w) == std::end(kWords)) return false;  // "" included
    const size_t n = words.size();
    return words[n - 1] == "KEY" || (n >= 2 && words[n - 1] == "BLOCK" && words[n - 2] == "KEY");
}

}  // namespace

std::string mask(const std::string& text) {
    const size_t newline = text.find('\n');
    std::string first = text.substr(0, newline);
    while (!first.empty() && first.back() == '\r') first.pop_back();
    const size_t lines = 1 + static_cast<size_t>(std::count(text.begin(), text.end(), '\n'));
    std::string out;
    if (is_pem_key_header(first)) {
        out = first;
    } else {
        std::vector<std::string> points;
        for (size_t i = 0; i < first.size();) {
            const unsigned char c = static_cast<unsigned char>(first[i]);
            const size_t n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
            points.push_back(first.substr(i, n));
            i += n;
        }
        const size_t n = points.size();
        const size_t keep = n >= 16 ? 4 : (n >= 8 ? 2 : 0), tail = n >= 16 ? 2 : 0;
        const size_t hidden = n - keep - tail;
        for (size_t i = 0; i < keep; ++i) out += points[i];
        out += std::string(std::min<size_t>(std::max<size_t>(hidden, 1), 12), '*');
        for (size_t i = n - tail; i < n; ++i) out += points[i];
    }
    if (lines > 1) out += " \xE2\x80\xA6 (" + std::to_string(lines) + " lines)";
    return out;
}

namespace {

std::string view_text(const std::vector<uint8_t>& b, int64_t a, int64_t z, bool ascii) {
    return ascii ? ascii_text(b.data() + a, static_cast<size_t>(z - a)) : text_of(b, a, z);
}

// Replacements that hide the parts of `hidden` inside [lo, hi) that no replacement of `done` covers yet: each such
// part is masked as a value of its own (widened to its token), or shown as a base64 blob. Parts already hidden are
// left alone, so that a view whose findings cover every marked byte is exactly what it would be without `hidden`.
void hide_rest(const std::vector<uint8_t>& b, const std::vector<HiddenSpan>& hidden, int64_t lo, int64_t hi, bool ascii,
               std::vector<Replacement>& done) {
    // `hidden` is sorted and disjoint: its ends are sorted too, and the first span ending after lo starts the scan.
    auto it =
        std::upper_bound(hidden.begin(), hidden.end(), lo, [](int64_t x, const HiddenSpan& h) { return x < h.end; });
    if (it == hidden.end() || it->start >= hi) return;
    std::vector<Range> covered;
    for (const Replacement& r : done)
        if (r.start < r.end) covered.push_back({r.start, r.end});
    std::sort(covered.begin(), covered.end());
    std::vector<Range> merged;
    for (const Range& c : covered) {
        if (!merged.empty() && c.first <= merged.back().second)
            merged.back().second = std::max(merged.back().second, c.second);
        else
            merged.push_back(c);
    }
    std::vector<Replacement> extra;
    for (; it != hidden.end() && it->start < hi; ++it) {
        int64_t a = std::max(it->start, lo);
        const int64_t z = std::min(it->end, hi);
        auto c =
            std::upper_bound(merged.begin(), merged.end(), a, [](int64_t x, const Range& r) { return x < r.second; });
        while (a < z) {
            if (c != merged.end() && c->first <= a) {  // covered from a on
                a = c->second;
                ++c;
                continue;
            }
            const int64_t piece = c != merged.end() ? std::min(z, c->first) : z;
            if (it->base64 && !ascii) {
                extra.push_back({a, piece, blob_marker(it->end - it->start)});
            } else {
                const Range w = widen_to_token(b, a, piece, lo, hi);
                extra.push_back({w.first, w.second, mask(view_text(b, w.first, w.second, ascii))});
            }
            a = piece;
        }
    }
    done.insert(done.end(), extra.begin(), extra.end());
}

}  // namespace

MaskedViews::MaskedViews(const std::vector<uint8_t>& bytes, const std::vector<Finding>& shown,
                         const std::vector<HiddenSpan>* hidden)
    : bytes_(bytes), shown_(shown), hidden_(hidden) {
    order_.resize(shown.size());
    for (size_t i = 0; i < order_.size(); ++i) order_[i] = i;
    std::stable_sort(order_.begin(), order_.end(), [&](size_t x, size_t y) { return shown[x].start < shown[y].start; });
    starts_.reserve(order_.size());
    for (size_t i : order_) starts_.push_back(shown[i].start);
    while (leaves_ < order_.size()) leaves_ *= 2;
    max_end_.assign(2 * leaves_, std::numeric_limits<int64_t>::min());
    for (size_t k = 0; k < order_.size(); ++k) max_end_[leaves_ + k] = shown[order_[k]].end;
    for (size_t node = leaves_ - 1; node >= 1; --node)
        max_end_[node] = std::max(max_end_[2 * node], max_end_[2 * node + 1]);
}

// A finding meets [lo, hi) when it starts before hi and ends after lo: among the findings that start before hi (a
// prefix of `order_`), the tree of the largest ends leads to those that end after lo, in O(log F) per finding found.
std::vector<size_t> MaskedViews::meeting(int64_t lo, int64_t hi) const {
    std::vector<size_t> out;
    const size_t below = static_cast<size_t>(std::lower_bound(starts_.begin(), starts_.end(), hi) - starts_.begin());
    if (below > 0) collect(1, 0, leaves_, below, lo, out);
    std::sort(out.begin(), out.end());
    return out;
}

void MaskedViews::collect(size_t node, size_t l, size_t r, size_t below, int64_t lo, std::vector<size_t>& out) const {
    if (l >= below || max_end_[node] <= lo) return;
    if (r - l == 1) {
        out.push_back(order_[l]);
        return;
    }
    const size_t mid = l + (r - l) / 2;
    collect(2 * node, l, mid, below, lo, out);
    collect(2 * node + 1, mid, r, below, lo, out);
}

std::string MaskedViews::render(int64_t lo, int64_t hi, const Finding& f, bool ascii) const {
    std::vector<Replacement> replacements;
    auto add = [&](const Finding& g) {
        if (!(g.start < hi && lo < g.end)) return;
        const int64_t a = std::max(g.start, lo), z = std::min(g.end, hi);
        if (!ascii && g.inside_base64) return replacements.push_back({a, z, blob_marker(g.end - g.start)});
        const Range w = widen_to_token(bytes_, a, z, lo, hi);
        replacements.push_back({w.first, w.second, mask(view_text(bytes_, w.first, w.second, ascii))});
    };
    for (size_t i : meeting(lo, hi)) add(shown_[i]);
    add(f);
    if (hidden_) hide_rest(bytes_, *hidden_, lo, hi, ascii, replacements);
    return splice(bytes_, lo, hi, std::move(replacements), ascii);
}

std::string MaskedViews::line(const Finding& f) const {
    const Range context = context_range(bytes_, f.start, f.end);  // the line, or its part around the finding
    const std::string line = cut(strip(render(context.first, context.second, f, false)), 160);
    return f.inside_base64 ? "(inside a base64) " + line : line;
}

std::string MaskedViews::string(const Finding& f) const {
    // Every finding of one string shows the same view, rendered once: the string with every finding in it masked.
    // That holds for a finding that one of `shown` covers (the finding itself, usually); another is rendered alone.
    bool covered = false;
    for (size_t i : meeting(f.start, f.end))
        covered = covered || (shown_[i].start <= f.start && f.end <= shown_[i].end);
    if (!covered || f.start >= f.end) return cut(render(f.string_start, f.string_end, f, true), 160);
    const auto key = std::make_pair(f.string_start, f.string_end);
    auto it = strings_.find(key);
    if (it == strings_.end()) it = strings_.emplace(key, cut(render(f.string_start, f.string_end, f, true), 160)).first;
    return it->second;
}

std::string masked_line(const std::vector<Finding>& others, const Finding& f, const std::vector<uint8_t>& b) {
    return MaskedViews(b, others).line(f);
}

std::string masked_string(const std::vector<Finding>& others, const Finding& f, const std::vector<uint8_t>& b) {
    return MaskedViews(b, others).string(f);
}

void merge_hidden(std::vector<HiddenSpan>& spans) {
    spans.erase(std::remove_if(spans.begin(), spans.end(), [](const HiddenSpan& h) { return h.end <= h.start; }),
                spans.end());
    std::sort(spans.begin(), spans.end(), [](const HiddenSpan& x, const HiddenSpan& y) {
        return x.start != y.start ? x.start < y.start : x.end < y.end;
    });
    std::vector<HiddenSpan> out;
    for (const HiddenSpan& h : spans) {
        if (!out.empty() && h.start <= out.back().end) {
            out.back().end = std::max(out.back().end, h.end);
            out.back().base64 = out.back().base64 || h.base64;
        } else {
            out.push_back(h);
        }
    }
    spans.swap(out);
}

}  // namespace pb::detect
