// purebyte serve: the local HTTP API (docs/api.md). Models are loaded once; requests are answered from memory (nothing
// is written to disk and no content is logged). Loopback only by default, with a Host check against DNS rebinding
// (whenever there is no token, and always on loopback), an Origin check and a custom header on form-like POSTs against
// web pages, and an optional bearer token. Archives are not expanded unless --archives is given: inference runs one
// request at a time and cannot be interrupted, and an archive can hold far more than its size.
//
//   GET /health            liveness, version, kernel, loaded models
//   GET /v1/models         what each loaded model is and declares
//   POST /v1/scan          one input, any size up to the model's limit (JSON; SARIF with ?format=sarif)
//   POST /v1/decide        one small input (up to 4 KB): the decision and the spans
//   POST /v1/redact        a redacted copy (models with typed spans), any size up to the model's limit
#include "http_request.h"  // first: it brings winsock2.h before windows.h

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>

#include "catalog.h"
#include "commands.h"
#include "core/digest.h"
#include "core/files.h"
#include "core/utf8.h"
#include "decision.h"
#include "output.h"
#include "sarif.h"

namespace cli {

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

// The bytes of body that the current request's route read, for the access log: -1 when no route read one. The routes
// read bodies themselves, so cpp-httplib's `req.body` stays empty; and cpp-httplib handles a request on one thread,
// from its route to its log line.
thread_local int64_t t_body_bytes = -1;

void send_error(httplib::Response& res, int status, const std::string& code, const std::string& message) {
    pb::json::Writer w;
    w.begin_object()
        .key("error")
        .begin_object()
        .field("code", code)
        .field("message", message)
        .end_object()
        .end_object();
    res.status = status;
    res.set_content(w.take() + "\n", "application/json");
}

// Compares in a time that does not depend on where the strings differ.
bool same_secret(const std::string& a, const std::string& b) {
    unsigned char diff = static_cast<unsigned char>(a.size() != b.size());
    for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i % (b.empty() ? 1 : b.size())]);
    return diff == 0;
}

std::string lowercase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// The fields that name a served model, inside an object the caller opened.
void write_model(pb::json::Writer& w, const Setup& s) {
    w.field("name", s.specialist.name).field("version", s.specialist.version).field("profile", s.profile);
}

class Server {
public:
    // `hosts`: the names a Host header may give (lowercase, IPv6 addresses in brackets), when `check_host`.
    Server(std::vector<Setup> served, std::string token, std::vector<std::string> hosts, bool check_host)
        : served_(std::move(served)), token_(std::move(token)), hosts_(std::move(hosts)), check_host_(check_host) {}

    const std::vector<Setup>& served() const { return served_; }
    const std::string& token() const { return token_; }
    bool check_host() const { return check_host_; }
    bool allowed_host(const std::string& name) const {
        return std::find(hosts_.begin(), hosts_.end(), lowercase(name)) != hosts_.end();
    }

    std::string models() const {
        pb::json::Writer w;
        w.begin_object().key("models").begin_array();
        for (const Setup& s : served_) {
            w.begin_object();
            write_model(w, s);
            w.key("files").begin_array();
            for (const ModelFile& f : s.specialist.members)
                w.begin_object().field("file", f.path).field("size", f.size).field("sha256", f.sha256).end_object();
            w.end_array().key("declaration").raw(pb_model_describe(s.specialist.members[0].handle.get())).end_object();
        }
        w.end_array().end_object();
        return w.take() + "\n";
    }

    // POST /v1/scan (decide_only = false) and /v1/decide (decide_only = true).
    void scan(const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& content,
              bool decide_only, uint64_t& received) {
        const auto started = Clock::now();
        Request r = read_request(req, content, decide_only ? Endpoint::Decide : Endpoint::Scan, received);
        const Setup& s = pick(r);
        check_input(r, s, "scan");
        if (decide_only && r.input.size() > kDecideLimit) throw HttpError{413, "too_large_for_decide", kDecideTooLarge};
        const std::string format = r.get("format", "json");
        if (format != "json" && format != "sarif")
            throw HttpError{400, "bad_parameter", "`format` is json or sarif, not `" + format + "`"};
        Setup local = s;  // the same model and session, with the request's own settings
        local.settings = settings_of(r, s);
        const NamedInput input{r.filename, std::move(r.input)};
        std::lock_guard<std::mutex> lock(inference_);  // one inference at a time: a session is not re-entrant
        if (decide_only) {
            const Decision d = decide(local, input);
            res.set_content(
                d.json.substr(0, d.json.size() - 1) + ",\"ms\":" + pb::json::number(ms_since(started)) + "}\n",
                "application/json");
            return;
        }
        const pb::json::Value out = detect(local.session.get(), local.detector.get(), {input}, local.settings);
        std::vector<Failure> failures;
        for (const pb::json::Value& p : out.get("failures")->array)
            failures.push_back({string_of(p, "file"), string_of(p, "reason")});
        if (format == "sarif")
            return res.set_content(sarif(out, input, failures, local) + "\n", "application/sarif+json");
        const std::vector<pb::json::Value>& findings = out.get("findings")->array;
        const int64_t total = static_cast<int64_t>(findings.size());
        const int64_t warnings = std::count_if(findings.begin(), findings.end(), is_warning);
        pb::json::Writer w;
        w.begin_object().key("model").begin_object();
        write_model(w, s);
        w.end_object().key("results").raw(pb::json::dump(*out.get("results")));
        w.key("findings").raw(pb::json::dump(*out.get("findings")));
        w.key("failures").begin_array();
        for (const Failure& f : failures) w.begin_object().field("file", f.file).field("reason", f.reason).end_object();
        w.end_array().key("stats").begin_object().field("findings", total);
        w.key("by_severity").begin_object().field("error", total - warnings).field("warning", warnings);
        w.end_object().end_object();
        w.field("ms", ms_since(started)).end_object();
        res.set_content(w.take() + "\n", "application/json");
    }

    // POST /v1/redact: the input checks of /v1/scan (empty_input, too_large), so that its work is bounded by the
    // model's input limit too.
    void redact(const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& content,
                uint64_t& received) {
        Request r = read_request(req, content, Endpoint::Redact, received);
        const Setup& s = pick(r);
        check_input(r, s, "redact");
        if (s.declaration.entities.empty())
            throw HttpError{400, "bad_parameter", "`" + s.specialist.name + "` has no typed spans to redact"};
        const DetectSettings d = settings_of(r, s);
        pb_redact_options o;
        pb_redact_options_init(&o);
        o.use_bias = d.has_bias ? 1 : 0;
        o.bias = d.bias;
        o.type_bias = d.type_bias.empty() ? nullptr : d.type_bias.data();
        o.type_bias_count = static_cast<uint32_t>(d.type_bias.size());
        o.min_confidence = d.min_confidence;
        const std::string types = r.get("types");
        o.types = types.empty() ? nullptr : types.c_str();
        o.name = r.filename.empty() ? nullptr : r.filename.c_str();
        const bool with_map = r.has("map") && truthy(r.get("map"), "map");
        o.flags = with_map ? PB_REDACT_WITH_MAP : 0u;
        pb_redaction* handle = nullptr;
        pb_error err;
        {
            std::lock_guard<std::mutex> lock(inference_);
            if (pb_redact(s.session.get(), s.detector.get(), r.input.data(), r.input.size(), &o, &handle, &err) !=
                PB_OK)
                throw std::runtime_error(err.message);
        }
        const std::unique_ptr<pb_redaction, void (*)(pb_redaction*)> redaction(handle, pb_redaction_free);
        size_t size = 0;
        const uint8_t* bytes = pb_redaction_output(handle, &size);
        const std::string text(reinterpret_cast<const char*>(bytes), size);
        pb::json::Writer w;
        w.begin_object();
        if (pb::utf8::sanitize(text) == text)
            w.field("redacted", text);
        else
            w.field("redacted_base64", pb::base64_encode(bytes, size));
        w.key("report").raw(pb_redaction_report(handle));
        if (with_map) w.key("map").raw(pb_redaction_map(handle));
        w.end_object();
        res.set_content(w.take() + "\n", "application/json");
    }

private:
    const Setup& pick(const Request& r) const {
        if (!r.has("model")) return served_[0];
        for (const Setup& s : served_)
            if (s.specialist.name == r.get("model")) return s;
        std::string names;
        for (const Setup& s : served_) names += (names.empty() ? "" : ", ") + s.specialist.name;
        throw HttpError{404, "unknown_model",
                        "no loaded model is called `" + r.get("model") + "` (loaded: " + names + ")"};
    }

    // An empty input, or one over the model's limit, is refused before any inference.
    static void check_input(const Request& r, const Setup& s, const char* what) {
        if (r.input.empty())
            throw HttpError{400, "empty_input", std::string("nothing to ") + what + ": the input is empty"};
        const uint64_t limit = pb_detector_max_input_bytes(s.detector.get());
        if (r.input.size() > limit)
            throw HttpError{413, "too_large", "this model takes at most " + std::to_string(limit) + " bytes per input"};
    }

    // Columns come from the bytes the profile analyzed: unknown for converted (UTF-16) text and archive members.
    static std::string sarif(const pb::json::Value& out, const NamedInput& input, const std::vector<Failure>& failures,
                             const Setup& s) {
        std::vector<SarifFinding> findings;
        const std::vector<pb::json::Value>& results = out.get("results")->array;
        size_t next = 0;
        for (const pb::json::Value& one : results) {
            const pb::json::Value* counters = one.get("counters");
            const bool plain = results.size() == 1 && !(counters && counters->get("utf16_converted"));
            for (size_t i = 0; i < one.get("spans")->array.size(); ++i, ++next) {
                const pb::json::Value& f = out.get("findings")->array.at(next);
                findings.push_back(
                    {&f, plain && f.get("line") ? code_point_column(input.bytes, number_of(f, "start")) : 0});
            }
        }
        return render_sarif(findings, failures, s.specialist.name, s.specialist.version, s.settings.reveal);
    }

    std::vector<Setup> served_;
    std::string token_;
    std::vector<std::string> hosts_;
    bool check_host_;
    std::mutex inference_;
};

bool is_loopback_name(const std::string& host) { return host == "127.0.0.1" || host == "localhost" || host == "[::1]"; }

// An Origin header (RFC 6454) naming a page served by this machine: http or https, 127.0.0.1, localhost or [::1], and
// any port. Anything else, "null" included, is a page of another site.
bool loopback_origin(const std::string& origin) {
    const std::string o = lowercase(origin);
    const size_t scheme = o.rfind("http://", 0) == 0 ? 7 : o.rfind("https://", 0) == 0 ? 8 : 0;
    if (scheme == 0) return false;
    const std::string rest = o.substr(scheme);
    const size_t bracket = rest.find(']');
    const size_t colon = rest.find(':', bracket == std::string::npos ? 0 : bracket);
    if (colon == std::string::npos) return is_loopback_name(rest);
    const std::string port = rest.substr(colon + 1);
    if (port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos) return false;
    return is_loopback_name(rest.substr(0, colon));
}

// The bodies a web page can POST to any address without the browser asking the server first (a CORS "simple
// request"): a form, plain text, or no type at all. A request with any other type, or with a header a page cannot add
// on its own, has been approved by a CORS preflight, which this server never grants.
bool page_could_send(const std::string& content_type) {
    const std::string type = media_type(content_type);
    return type.empty() || type == "application/x-www-form-urlencoded" || type == "multipart/form-data" ||
           type == "text/plain";
}

// `Authorization: Bearer <token>`, the scheme in any case (RFC 9110, section 11.1), the token compared in a time that
// does not depend on where it differs.
bool authorized(const httplib::Request& req, const std::string& token) {
    const std::string value = req.get_header_value("Authorization");
    const size_t space = value.find(' ');
    if (space == std::string::npos || lowercase(value.substr(0, space)) != "bearer") return false;
    const size_t start = value.find_first_not_of(' ', space);
    return same_secret(start == std::string::npos ? std::string() : value.substr(start), token);
}

// Every request, before its route: the Host check (against DNS rebinding: on loopback, and whenever there is no
// token), the Origin check, the token, the header that form-like POSTs need, and the size of /v1/decide bodies.
httplib::Server::HandlerResponse admit(const Server& server, const httplib::Request& req, httplib::Response& res) {
    if (server.check_host() && req.has_header("Host")) {
        std::string h = req.get_header_value("Host");
        h = !h.empty() && h[0] == '[' ? h.substr(0, h.find(']') + 1) : h.substr(0, h.find(':'));
        if (!server.allowed_host(h)) {
            send_error(res, 403, "bad_host",
                       "this server only answers requests addressed to 127.0.0.1, localhost, the address it listens "
                       "on or a name given with --allow-host");
            return httplib::Server::HandlerResponse::Handled;
        }
    }
    // Browsers name the page behind a request in Origin; programs such as curl send none.
    if (req.has_header("Origin") && !loopback_origin(req.get_header_value("Origin"))) {
        send_error(res, 403, "bad_origin", "this server does not answer web pages of other sites");
        return httplib::Server::HandlerResponse::Handled;
    }
    if (!server.token().empty() && req.path != "/health" && !authorized(req, server.token())) {
        send_error(res, 401, "unauthorized", "missing or wrong `Authorization: Bearer <token>`");
        return httplib::Server::HandlerResponse::Handled;
    }
    // A form or plain text could have been sent by any web page the user opens (and an old browser may leave Origin
    // out): such a POST needs X-PureByte, or the token, which no page can add without the server's consent.
    if (req.method == "POST" && server.token().empty() && !req.has_header("X-PureByte") &&
        page_could_send(req.get_header_value("Content-Type"))) {
        send_error(res, 403, "missing_header",
                   "a form, plain-text or untyped body needs the header `X-PureByte: 1` (any web page could send "
                   "one); or send JSON with `Content-Type: application/json`, or raw bytes with `Content-Type: "
                   "application/octet-stream`");
        return httplib::Server::HandlerResponse::Handled;
    }
    // A body announced over the limit is refused before it is read; a chunked one, when its route has read that much.
    if (req.path == "/v1/decide" && req.get_header_value_u64("Content-Length") > kDecideBodyLimit) {
        send_error(res, 413, "too_large_for_decide", kDecideTooLarge);
        return httplib::Server::HandlerResponse::Handled;
    }
    return httplib::Server::HandlerResponse::Unhandled;
}

// A route that reads its own body, and whose failures become the JSON errors of docs/api.md (details of internal
// failures go to the log only).
template <class Handler>
httplib::Server::HandlerWithContentReader route(Handler handler) {
    return [handler](const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& content) {
        uint64_t received = 0;
        try {
            handler(req, res, content, received);
        } catch (const HttpError& e) {
            send_error(res, e.status, e.code, e.message);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "purebyte: request failed: %s\n", e.what());
            send_error(res, 500, "engine_failure", "the engine failed on this input (details in the server log)");
        }
        t_body_bytes = static_cast<int64_t>(received);
    };
}

// The methods a known path takes, for `405 method_not_allowed` (with an Allow header); nullptr for an unknown path.
const char* allowed_methods(const std::string& path) {
    if (path == "/v1/scan" || path == "/v1/decide" || path == "/v1/redact") return "POST";
    if (path == "/health" || path == "/v1/models") return "GET, HEAD";
    return nullptr;
}

// A path as the access log writes it: control characters and backslashes escaped (\n, \x1b, \\), so that a request
// cannot write a line of its own into the log.
std::string printable(const std::string& s) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    for (const unsigned char c : s) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c < 0x20 || c == 0x7f) {
            out += "\\x";
            out += kHex[c >> 4];
            out += kHex[c & 15];
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

// The token: --token, the first line of --token-file, else PUREBYTE_SERVE_TOKEN; "" when none is given. A token that
// is given but empty is refused, wherever it comes from: an unset variable in `--token "$TOKEN"`, or an empty
// PUREBYTE_SERVE_TOKEN, must never start a server without authentication.
std::string token_of(const Args& a) {
    if (a.has("token") && a.has("token-file")) throw UsageError("--token and --token-file are alternatives: give one");
    const auto blank = [](const std::string& t) { return t.find_first_not_of(" \t\r\n") == std::string::npos; };
    if (a.has("token")) {
        std::fprintf(stderr,
                     "purebyte: warning: other users of this machine can read --token in the list of processes; "
                     "prefer --token-file FILE or the PUREBYTE_SERVE_TOKEN variable\n");
        if (blank(a.text("token")))
            throw UsageError("--token is empty: give the token (or leave --token out to serve without one)", 2);
        return a.text("token");
    }
    if (a.has("token-file")) {
        const std::string path = a.text("token-file");
        std::vector<uint8_t> bytes;
        try {
            bytes = pb::read_file(path);
        } catch (const std::exception&) {
            throw UsageError("cannot read the token file " + display_path(path), 2);
        }
        std::string token(bytes.begin(), std::find(bytes.begin(), bytes.end(), uint8_t('\n')));
        if (token.compare(0, 3, "\xEF\xBB\xBF") == 0) token.erase(0, 3);  // the byte order mark some editors write
        if (token.find('\0') != std::string::npos)
            throw UsageError("the token file " + display_path(path) + " is not text (UTF-16?): save it as UTF-8", 2);
        token.erase(0, token.find_first_not_of(" \t\r"));
        token.erase(token.find_last_not_of(" \t\r") + 1);
        if (token.empty())
            throw UsageError("the token file " + display_path(path) + " has no token on its first line", 2);
        return token;
    }
    const char* variable = std::getenv("PUREBYTE_SERVE_TOKEN");
    if (!variable) return "";
    if (blank(variable))
        throw UsageError(
            "PUREBYTE_SERVE_TOKEN is set but empty: set it to the token (or unset it to serve without one)", 2);
    return variable;
}

// An address as a Host header or a URL writes it: IPv6 addresses in brackets.
std::string url_host(const std::string& host) {
    return host.find(':') != std::string::npos && host[0] != '[' ? "[" + host + "]" : host;
}

}  // namespace

int command_serve(const std::vector<std::string>& argv) {
    std::vector<OptionSpec> spec = model_options();
    for (OptionSpec& o : spec)
        if (std::string(o.name) == "model")
            o = {"model", "NAME", "a model to serve (repeatable; default: secrets-code)", true};
    spec.insert(spec.end(),
                {
                    {"host", "ADDRESS", "address to listen on (default 127.0.0.1)"},
                    {"port", "N", "port (default 8421)"},
                    {"token", "TOKEN", "require `Authorization: Bearer TOKEN` on every endpoint but /health"},
                    {"token-file", "FILE", "the same, with the token on the first line of FILE (out of process lists)"},
                    {"allow-host", "NAME", "also answer requests addressed to NAME (repeatable)", true},
                    {"archives", nullptr, "scan the members of zip, gzip and tar inputs (off by default)"},
                    {"http-threads", "N", "threads that handle connections (default 4)"},
                    {"max-body-mb", "N", "largest request body in MiB (default 80)"},
                    {"quiet", nullptr, "no access log on standard error"},
                    {"help", nullptr, "show this help"},
                });
    const Args a(argv, 2, spec);
    if (a.has("help")) {
        std::fputs(("usage: purebyte serve [--model NAME ...] [options]\n\nThe local HTTP API (docs/api.md). Without "
                    "--token or --token-file, the token is PUREBYTE_SERVE_TOKEN when set.\n\n" +
                    options_help(spec))
                       .c_str(),
                   stdout);
        return 0;
    }
    if (!a.positional().empty()) throw UsageError("serve takes no argument: give models with --model");
    const std::string token = token_of(a);
    std::vector<std::string> names = a.all("model");
    if (names.empty()) names.push_back("secrets-code");
    if (names.size() > 1 && a.has("ensemble"))
        throw UsageError("--ensemble belongs to one model: serve that model alone");
    std::vector<Setup> served;
    std::shared_ptr<pb_session> session;  // shared by every model: requests are served one at a time
    for (const std::string& name : names) {
        std::vector<std::string> model_argv = {argv[0], argv[1], "--model",
                                               name};  // the common options, for each model
        for (const char* key :
             {"profile", "bias", "type-bias", "votes", "min-confidence", "threads", "intra-threads", "kernel"})
            if (a.has(key)) model_argv.insert(model_argv.end(), {std::string("--") + key, a.text(key)});
        if (a.has("no-ensemble")) model_argv.push_back("--no-ensemble");
        for (const std::string& file : a.all("ensemble")) model_argv.insert(model_argv.end(), {"--ensemble", file});
        Setup s = setup(Args(model_argv, 2, model_options()), name, session);
        s.settings.expand_archives = a.has("archives");
        session = s.session;
        for (const Setup& other : served)
            if (other.specialist.name == s.specialist.name) throw UsageError("model `" + name + "` given twice");
        served.push_back(std::move(s));
    }
    const std::string host = a.text("host", "127.0.0.1");
    const int port = a.integer("port", 8421, 1, 65535);
    const bool loopback = host == "127.0.0.1" || host == "localhost" || host == "::1";
    // The names a request may be addressed to: loopback, the address the server listens on (not a wildcard), and
    // --allow-host. Checked on loopback and whenever there is no token: a page that rebinds its own name to this
    // machine sends that name, never these.
    std::vector<std::string> hosts = {"127.0.0.1", "localhost", "[::1]"};
    if (host != "0.0.0.0" && host != "::" && host != "[::]") hosts.push_back(lowercase(url_host(host)));
    for (const std::string& name : a.all("allow-host")) {
        const bool bracketed = name.size() > 2 && name.front() == '[' && name.back() == ']';  // an IPv6 address
        if (name.empty() || name.find_first_of(" /") != std::string::npos ||
            (name.find(':') != std::string::npos && !bracketed))
            throw UsageError("--allow-host takes a host name or an address, without a port: not `" + name + "`");
        hosts.push_back(lowercase(name));
    }
    Server server(std::move(served), token, std::move(hosts), loopback || token.empty());

    httplib::Server http;
    const int http_threads = a.integer("http-threads", 4, 1, 256);
    http.new_task_queue = [http_threads] { return new httplib::ThreadPool(static_cast<size_t>(http_threads)); };
    http.set_payload_max_length(static_cast<size_t>(a.integer("max-body-mb", 80, 1, 4096)) << 20);
    http.set_read_timeout(60, 0);
    http.set_write_timeout(60, 0);
    // The port is the server's alone. cpp-httplib's default, SO_REUSEADDR (SO_REUSEPORT where it exists), would let
    // another program bind it too and receive requests (on Windows, any program; elsewhere, one of the same user).
    // SO_EXCLUSIVEADDRUSE forbids it on Windows; elsewhere SO_REUSEADDR alone only lets a restart reuse the port while
    // the connections of the previous run close.
    http.set_socket_options([](socket_t sock) {  // socket_t: cpp-httplib's, SOCKET on Windows and int elsewhere
#ifdef _WIN32
        httplib::set_socket_opt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#else
        httplib::set_socket_opt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
    });
    const auto started = Clock::now();
    http.set_pre_routing_handler(
        [&server](const httplib::Request& req, httplib::Response& res) { return admit(server, req, res); });
    http.Get("/health", [&server, started](const httplib::Request&, httplib::Response& res) {
        const Setup& first = server.served()[0];
        pb::json::Writer w;
        w.begin_object().field("status", "ok").field("version", pb_version());
        w.field("kernel", pb_session_kernel(first.session.get()))
            .field("threads", first.threads)
            .key("models")
            .begin_array();
        for (const Setup& s : server.served()) w.string(s.specialist.name);
        w.end_array().field("uptime_s", std::chrono::duration<double>(Clock::now() - started).count()).end_object();
        res.set_content(w.take() + "\n", "application/json");
    });
    const std::string models = server.models();
    http.Get("/v1/models", [models](const httplib::Request&, httplib::Response& res) {
        res.set_content(models, "application/json");
    });
    using Content = httplib::ContentReader;
    http.Post("/v1/scan", route([&server](const httplib::Request& q, httplib::Response& s, const Content& c,
                                          uint64_t& n) { server.scan(q, s, c, false, n); }));
    http.Post("/v1/decide", route([&server](const httplib::Request& q, httplib::Response& s, const Content& c,
                                            uint64_t& n) { server.scan(q, s, c, true, n); }));
    http.Post("/v1/redact", route([&server](const httplib::Request& q, httplib::Response& s, const Content& c,
                                            uint64_t& n) { server.redact(q, s, c, n); }));
    // A known path asked with another method (PUT /v1/scan, POST /health...) is 405, not 404.
    http.set_error_handler([](const httplib::Request& req, httplib::Response& res) {
        if (!res.body.empty()) return;
        const char* allow = res.status == 404 ? allowed_methods(req.path) : nullptr;
        if (allow) {
            res.set_header("Allow", allow);
            send_error(res, 405, "method_not_allowed",
                       std::string(allow) == "POST"
                           ? "POST the bytes (JSON {\"text\": ...}, a raw application/octet-stream body, or a "
                             "multipart `file` part with `X-PureByte: 1`)"
                           : "this path only answers GET");
            return;
        }
        const int s = res.status;
        send_error(res, s,
                   s == 404   ? "not_found"
                   : s == 405 ? "method_not_allowed"
                   : s == 413 ? "payload_too_large"
                              : "bad_request",
                   s == 413 ? "the request body is larger than --max-body-mb" : "HTTP " + std::to_string(s));
    });
    if (!a.has("quiet"))
        http.set_logger([](const httplib::Request& req, const httplib::Response& res) {
            const size_t in = t_body_bytes >= 0 ? static_cast<size_t>(t_body_bytes) : req.body.size();
            t_body_bytes = -1;
            std::fprintf(stderr, "%s %s -> %d (%zu B in, %zu B out)\n", printable(req.method).c_str(),
                         printable(req.path).c_str(), res.status, in, res.body.size());
        });
    if (!http.bind_to_port(host, port)) throw UsageError("cannot listen on " + host + ":" + std::to_string(port), 2);
    if (!loopback)
        std::fprintf(stderr, "purebyte: WARNING: listening on %s, not on loopback: anyone who reaches it can scan\n",
                     host.c_str());
    std::fprintf(stderr, "purebyte: listening on http://%s:%d (kernel %s; %s)\n", url_host(host).c_str(), port,
                 pb_session_kernel(server.served()[0].session.get()),
                 token.empty() ? "no token" : "a bearer token is required");
    std::fflush(stderr);
    return http.listen_after_bind() ? 0 : 2;
}

}  // namespace cli
