#include "core/json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/utf8.h"

namespace pb::json {

std::string quote(const std::string& raw) {
    const std::string s = utf8::sanitize(raw);
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (const unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20 || c == 0x7f) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
    return out;
}

// snprintf and strtod follow the C locale of the process, which a host application may have changed: its decimal
// separator may be ',' or a character of several bytes. Neither conversion below depends on it, and neither calls
// localeconv(), whose result other threads may overwrite.

double parse_number(const char* text, size_t* consumed) {
    // RFC 8259: -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)?  The digits are handed to strtod without the point, as
    // "<digits>e<exponent>", so that the locale's decimal separator never matters; strtod rounds that exactly as it
    // would the text itself.
    auto digit = [&](size_t k) { return text[k] >= '0' && text[k] <= '9'; };
    size_t i = 0;
    std::string significand;
    if (text[i] == '-') significand += text[i++];
    if (text[i] == '0') {
        significand += text[i++];
    } else if (text[i] >= '1' && text[i] <= '9') {
        while (digit(i)) significand += text[i++];
    } else {
        *consumed = 0;
        return 0.0;
    }
    long long exponent = 0;  // of the significand's last digit
    if (text[i] == '.' && digit(i + 1)) {
        for (++i; digit(i); ++i) {
            significand += text[i];
            --exponent;
        }
    }
    if ((text[i] == 'e' || text[i] == 'E')) {
        size_t k = i + 1;
        const bool negative = text[k] == '-';
        if (text[k] == '-' || text[k] == '+') ++k;
        if (digit(k)) {
            long long e = 0;
            for (; digit(k); ++k) e = std::min(e * 10 + (text[k] - '0'), 1000000000LL);  // beyond: 0 or infinity
            exponent += negative ? -e : e;
            i = k;
        }
    }
    *consumed = i;
    const std::string plain = significand + "e" + std::to_string(exponent);
    return std::strtod(plain.c_str(), nullptr);
}

std::string number(double v) {
    if (!std::isfinite(v)) return "null";
    if (v == 0.0) return std::signbit(v) ? "-0.0" : "0";
    char buf[64];
    for (int precision = 1; precision <= 17; ++precision) {
        std::snprintf(buf, sizeof buf, "%.*g", precision, v);
        if (std::strtod(buf, nullptr) == v) break;  // both in the locale's notation
    }
    // Everything but digits, signs and the exponent mark is the locale's decimal separator: it becomes one '.'.
    std::string out;
    bool separator = false;
    for (const char* p = buf; *p; ++p) {
        const bool plain = (*p >= '0' && *p <= '9') || *p == '+' || *p == '-' || *p == 'e' || *p == 'E';
        if (plain) {
            out += *p;
        } else if (!separator) {
            out += '.';
        }
        separator = !plain;
    }
    return out;
}

void Writer::separate() {
    if (after_key_) {
        after_key_ = false;
        return;
    }
    if (!first_.empty()) {
        if (!first_.back()) out_ += ',';
        first_.back() = false;
    }
}

Writer& Writer::begin_object() {
    separate();
    out_ += '{';
    first_.push_back(true);
    return *this;
}

Writer& Writer::end_object() {
    out_ += '}';
    first_.pop_back();
    return *this;
}

Writer& Writer::begin_array() {
    separate();
    out_ += '[';
    first_.push_back(true);
    return *this;
}

Writer& Writer::end_array() {
    out_ += ']';
    first_.pop_back();
    return *this;
}

Writer& Writer::key(const std::string& k) {
    separate();
    out_ += quote(k);
    out_ += ':';
    after_key_ = true;
    return *this;
}

Writer& Writer::string(const std::string& s) {
    separate();
    out_ += quote(s);
    return *this;
}

Writer& Writer::integer(int64_t v) {
    separate();
    out_ += std::to_string(v);
    return *this;
}

Writer& Writer::real(double v) {
    separate();
    out_ += number(v);
    return *this;
}

Writer& Writer::boolean(bool v) {
    separate();
    out_ += v ? "true" : "false";
    return *this;
}

Writer& Writer::null() {
    separate();
    out_ += "null";
    return *this;
}

Writer& Writer::raw(const std::string& json) {
    separate();
    out_ += json;
    return *this;
}

const Value* Value::get(const std::string& key) const {
    for (const auto& kv : object)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

namespace {

class Parser {
public:
    explicit Parser(const std::string& text) : s_(text) {}

    bool document(Value& out, std::string& error) {
        const bool ok = value(out, 0) && (skip_space(), i_ == s_.size() || fail("trailing data"));
        if (!ok) error = error_;
        return ok;
    }

private:
    void skip_space() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
    }
    bool fail(const char* what) {
        if (error_.empty()) error_ = std::string(what) + " at byte " + std::to_string(i_);
        return false;
    }
    bool literal(const char* word) {
        const std::string w(word);
        if (s_.compare(i_, w.size(), w) != 0) return fail("unexpected character");
        i_ += w.size();
        return true;
    }
    bool hex4(uint32_t& v) {
        if (i_ + 4 > s_.size()) return fail("truncated \\u escape");
        v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_++];
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v |= static_cast<uint32_t>(c - 'A' + 10);
            else
                return fail("bad \\u escape");
        }
        return true;
    }
    bool string(std::string& out) {
        ++i_;  // opening quote
        while (i_ < s_.size()) {
            const unsigned char c = static_cast<unsigned char>(s_[i_++]);
            if (c == '"') return true;
            if (c < 0x20) return fail("control character in string");
            if (c != '\\') {
                out += static_cast<char>(c);
                continue;
            }
            if (i_ >= s_.size()) break;
            const char e = s_[i_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        uint32_t low;
                        if (i_ + 2 > s_.size() || s_[i_] != '\\' || s_[i_ + 1] != 'u') return fail("lone surrogate");
                        i_ += 2;
                        if (!hex4(low)) return false;
                        if (low < 0xDC00 || low > 0xDFFF) return fail("lone surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail("lone surrogate");
                    }
                    utf8::append(out, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool value(Value& v, int depth) {
        if (depth > 64) return fail("nesting too deep");
        skip_space();
        if (i_ >= s_.size()) return fail("unexpected end");
        const char c = s_[i_];
        if (c == '{') {
            v.type = Value::Type::Object;
            ++i_;
            skip_space();
            if (i_ < s_.size() && s_[i_] == '}') return ++i_, true;
            for (;;) {
                skip_space();
                if (i_ >= s_.size() || s_[i_] != '"') return fail("expected a key");
                std::string key;
                if (!string(key)) return false;
                skip_space();
                if (i_ >= s_.size() || s_[i_] != ':') return fail("expected ':'");
                ++i_;
                Value item;
                if (!value(item, depth + 1)) return false;
                v.object.emplace_back(std::move(key), std::move(item));
                skip_space();
                if (i_ < s_.size() && s_[i_] == ',') {
                    ++i_;
                    continue;
                }
                if (i_ < s_.size() && s_[i_] == '}') return ++i_, true;
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v.type = Value::Type::Array;
            ++i_;
            skip_space();
            if (i_ < s_.size() && s_[i_] == ']') return ++i_, true;
            for (;;) {
                Value item;
                if (!value(item, depth + 1)) return false;
                v.array.push_back(std::move(item));
                skip_space();
                if (i_ < s_.size() && s_[i_] == ',') {
                    ++i_;
                    continue;
                }
                if (i_ < s_.size() && s_[i_] == ']') return ++i_, true;
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            v.type = Value::Type::String;
            return string(v.string);
        }
        if (c == 't') {
            v.type = Value::Type::Bool;
            v.boolean = true;
            return literal("true");
        }
        if (c == 'f') {
            v.type = Value::Type::Bool;
            v.boolean = false;
            return literal("false");
        }
        if (c == 'n') {
            v.type = Value::Type::Null;
            return literal("null");
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            size_t consumed = 0;
            v.type = Value::Type::Number;
            v.number = parse_number(s_.c_str() + i_, &consumed);
            if (consumed == 0) return fail("bad number");
            i_ += consumed;
            return true;
        }
        return fail("unexpected character");
    }

    const std::string& s_;
    size_t i_ = 0;
    std::string error_;
};

}  // namespace

bool parse(const std::string& text, Value& out, std::string& error) { return Parser(text).document(out, error); }

namespace {

void dump_into(Writer& w, const Value& v) {
    switch (v.type) {
        case Value::Type::Null: w.null(); break;
        case Value::Type::Bool: w.boolean(v.boolean); break;
        case Value::Type::Number:
            // In range before the conversion (a double beyond int64 converts with undefined behaviour).
            if (std::isfinite(v.number) && std::fabs(v.number) < 9.0e15 && v.number == std::trunc(v.number))
                w.integer(static_cast<int64_t>(v.number));
            else
                w.real(v.number);
            break;
        case Value::Type::String: w.string(v.string); break;
        case Value::Type::Array:
            w.begin_array();
            for (const Value& item : v.array) dump_into(w, item);
            w.end_array();
            break;
        case Value::Type::Object:
            w.begin_object();
            for (const auto& kv : v.object) {
                w.key(kv.first);
                dump_into(w, kv.second);
            }
            w.end_object();
            break;
    }
}

}  // namespace

std::string dump(const Value& value) {
    Writer w;
    dump_into(w, value);
    return w.take();
}

}  // namespace pb::json
