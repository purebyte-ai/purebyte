// What a request to `purebyte serve` asks for (docs/api.md): the input (a raw body, JSON {"text"|"base64"}, or a
// multipart `file` part or `text` field) and its parameters (query string, JSON fields or form fields), checked
// against the parameters of its endpoint.
#pragma once

#include "http.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "detection.h"
#include "setup.h"

namespace cli {

// A request that cannot be served: its HTTP status and the error code and message of docs/api.md.
struct HttpError {
    int status;
    std::string code, message;
};

// The endpoints that take an input: each has its own parameters.
enum class Endpoint { Scan, Decide, Redact };

// /v1/decide takes at most 4096 bytes of input; its body (the input, or JSON holding it) is not read past 64 KiB.
constexpr uint64_t kDecideBodyLimit = 64u << 10;
extern const char* const kDecideTooLarge;  // the message of `too_large_for_decide`

struct Request {
    std::vector<uint8_t> input;
    std::string filename;
    std::multimap<std::string, std::string> params;

    std::string get(const std::string& key, const std::string& fallback = "") const;
    bool has(const std::string& key) const { return params.count(key) > 0; }
};

// Reads the body through `content` (the route reads it itself: cpp-httplib then never parses a form-encoded body into
// parameters, which this server does not use and which cost about ten times the body in memory) and the parameters.
// `received` counts the bytes of body read so far, also when an error is thrown. Throws HttpError on a body or a
// parameter it cannot accept.
Request read_request(const httplib::Request& req, const httplib::ContentReader& content, Endpoint endpoint,
                     uint64_t& received);

// The media type of a Content-Type value, lowercase and without its parameters: "Application/JSON; charset=utf-8" is
// "application/json" (RFC 9110, section 8.3.1).
std::string media_type(const std::string& content_type);

// A yes/no parameter.
bool truthy(const std::string& value, const std::string& name);

// The request's detection settings, on top of the served model's own.
DetectSettings settings_of(const Request& r, const Setup& s);

}  // namespace cli
