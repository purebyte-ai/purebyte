// Minimal JSON: a writer that produces compact, valid UTF-8 JSON, and a depth-limited parser of RFC 8259 JSON (the
// bytes of strings are passed through; the writer replaces invalid UTF-8). Numbers are converted the same way under
// every locale of the process.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pb::json {

// A JSON string literal. Invalid UTF-8 in `s` becomes U+FFFD so that the output is always valid JSON.
std::string quote(const std::string& s);
// The shortest decimal that reads back as the same double ("0.97", not "0.96999999999999997"); NaN/Inf -> null.
std::string number(double v);
// Reads the longest prefix of `text` that is a number of RFC 8259 syntax, whatever the process locale; `consumed`
// receives its length (0 = not a number).
double parse_number(const char* text, size_t* consumed);

class Writer {
public:
    Writer& begin_object();
    Writer& end_object();
    Writer& begin_array();
    Writer& end_array();
    Writer& key(const std::string& k);
    Writer& string(const std::string& s);
    Writer& integer(int64_t v);
    Writer& real(double v);
    Writer& boolean(bool v);
    Writer& null();
    Writer& raw(const std::string& json);  // an already serialised value

    // key + value in one call
    Writer& field(const std::string& k, const std::string& s) { return key(k).string(s); }
    Writer& field(const std::string& k, const char* s) { return key(k).string(s); }
    Writer& field(const std::string& k, int64_t v) { return key(k).integer(v); }
    Writer& field(const std::string& k, int v) { return key(k).integer(v); }
    Writer& field(const std::string& k, double v) { return key(k).real(v); }
    Writer& field(const std::string& k, bool v) { return key(k).boolean(v); }

    const std::string& str() const { return out_; }
    std::string take() { return std::move(out_); }

private:
    void separate();
    std::string out_;
    std::vector<bool> first_;  // per open container: no element written yet
    bool after_key_ = false;
};

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;  // in document order

    const Value* get(const std::string& key) const;
    bool is(Type t) const { return type == t; }
};

// Parses `text`; on failure returns false and describes the problem in `error`.
bool parse(const std::string& text, Value& out, std::string& error);

// Serialises a parsed value back to compact JSON (object members in document order).
std::string dump(const Value& value);

}  // namespace pb::json
