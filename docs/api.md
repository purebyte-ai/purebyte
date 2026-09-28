# API reference

Three ways to call PureByte from your own code. All of them run the same library and return the same results.

| Interface | Use it when | Section |
|---|---|---|
| HTTP | Any language, one long-running process, small decisions in milliseconds | [HTTP API](#http-api) |
| C | Embedding the runtime in a native program | [C API](#c-api) |
| Python | Scripts, notebooks, services in Python | [Python](#python) |

The result format is shared by all three and defined in [spec/OUTPUT.md](../spec/OUTPUT.md). Its main parts are
summarized in [customize.md](customize.md#the-generic-output).

## HTTP API

```bash
purebyte serve --model secrets-code --model secrets-bin      # http://127.0.0.1:8421
```

Models are loaded once, at start-up, and requests are answered from memory. Inference runs one request at a time,
with all the worker threads of the server (`--threads`); `--http-threads` only sets how many connections are handled
at once. An inference cannot be interrupted, so archives are scanned as they are unless the server was started with
`--archives` (an archive expands to far more than its own size). Server options: [cli.md](cli.md#serve).

### Endpoints

| Method and path | Purpose | Response |
|---|---|---|
| `GET /health` | Liveness | `{status, version, kernel, threads, models, uptime_s}`; never needs the token |
| `GET /v1/models` | The loaded models | `{models: [{name, version, profile, files: [{file, size, sha256}], declaration}]}`, where `declaration` is what the main model file declares |
| `POST /v1/scan` | One input of any size up to the model's limit | `{model: {name, version, profile}, results, findings, failures, stats, ms}`, or SARIF 2.1.0 with `format=sarif` |
| `POST /v1/decide` | One small input (up to 4,096 bytes) | `{decision, model, result, findings, ms}`: `decision` is `positive` or `negative` |
| `POST /v1/redact` | A redacted copy of the input, with a model that has typed spans | `{redacted, report}` (`redacted_base64` instead of `redacted` when the output is not valid UTF-8), plus `map` on request |

`results`, `result` and `findings` follow [spec/OUTPUT.md](../spec/OUTPUT.md); `failures` lists what could not be
analyzed (with `--archives`, for example an archive member over the limits); `stats` is `{findings, by_severity:
{error, warning}}`; `ms` is the time the server spent on the request. Every finding has a `severity`, `error` or
`warning`: with the `secrets-code` profile, an input whose `filename` is a test, example or documentation path
(`tests/app.cfg`, `docs/setup.md`) gets warnings, and SARIF uses the severity as each result's `level`
([spec/OUTPUT.md](../spec/OUTPUT.md#51-severity)). Send `filename` relative to the project's root: the whole value
counts, so the folders of an absolute path (`/home/me/test/repo/...`) would count too. The redaction report and map
are specified in [spec/OUTPUT.md](../spec/OUTPUT.md#9-redaction-output-report-and-map).

### Sending an input

The body of `scan`, `decide` and `redact` can be:

- the raw bytes, with any content type other than the two below (for example `application/octet-stream`);
- JSON (`Content-Type: application/json`, in any case and with or without parameters such as `charset`):
  `{"text": "..."}` for text, or `{"base64": "..."}` for any bytes, with the parameters as extra fields;
- `multipart/form-data` with one `file` part (its file name becomes `filename`) or one `text` field, with the
  parameters as form fields.

The body is read as the input and nothing else: a form-encoded body (`application/x-www-form-urlencoded`) is never
parsed into parameters.

A web page can send a form or plain text to any address without asking the browser first, so a body of type
`multipart/form-data`, `application/x-www-form-urlencoded` or `text/plain`, or with no type at all, also needs the
header `X-PureByte` (any value, such as `1`) or the server's token; without either it is refused with `403
missing_header`. JSON, and raw bytes sent as `application/octet-stream`, need nothing more. See [Security](#security).

Parameters go in the query string, or as JSON or form fields. A parameter that the endpoint does not take (an unknown
one, or one of another endpoint, such as `map` on `/v1/scan`) is refused with `400 bad_parameter`.

| Parameter | Endpoints | Meaning |
|---|---|---|
| `model` | all | Which loaded model answers, by name (default: the first one given to `serve`) |
| `filename` | all | A name for the input, shown in the results and as the SARIF location (default: none) |
| `format` | `scan` | `json` (default) or `sarif` |
| `bias` | all | The operating point, as `--bias` ([cli.md](cli.md#choosing-a-model)) |
| `votes` | `scan`, `decide` | Ensemble votes a finding needs, from 1 to the number of members |
| `min_confidence` | all | Drop spans below this confidence |
| `ensemble` | `scan`, `decide` | `no` to use only the first model of the ensemble |
| `reveal` | `scan`, `decide` | `yes` to also return the detected values in clear |
| `per_model` | `scan`, `decide` | `yes` to add every ensemble member's own findings |
| `prefilter`, `early_exit` | `scan`, `decide` | As the CLI options of the same name ([cli.md](cli.md#scan)) |
| `strings_only` | `scan`, `decide` | `secrets-binary` profile: `no` to also return findings that touch no printable string, as the CLI's `--all-bytes`. Default `yes` |
| `query` | `scan`, `decide` | A field to extract, for query-conditioned models; repeatable (in JSON: `"queries": [...]`) |
| `types` | `redact` | Comma-separated entity types to redact (default: all) |
| `map` | `redact` | `yes` to return the reversible map. It holds the original values: treat the response as a secret |

Yes/no parameters accept `yes`, `no`, `true`, `false`, `on`, `off`, `1` and `0`; in JSON they can also be booleans.

### Examples

```bash
curl -s http://127.0.0.1:8421/health

curl -s http://127.0.0.1:8421/v1/decide \
  -H 'Content-Type: application/json' \
  -d '{"text": "export DB_PASSWORD=\"Tr0ub4dor&3xyz\""}'

curl -s 'http://127.0.0.1:8421/v1/scan?filename=app.js' \
  -H 'Content-Type: application/octet-stream' --data-binary @app.js

curl -s 'http://127.0.0.1:8421/v1/scan?model=secrets-bin' \
  -H 'Content-Type: application/octet-stream' --data-binary @app.exe

curl -s 'http://127.0.0.1:8421/v1/scan?format=sarif&filename=.env' \
  -H 'Content-Type: application/octet-stream' --data-binary @.env

curl -s 'http://127.0.0.1:8421/v1/redact?filename=app.log' \
  -H 'Content-Type: application/octet-stream' --data-binary @app.log

curl -s http://127.0.0.1:8421/v1/scan -H 'X-PureByte: 1' -F file=@config.yaml
```

`curl --data-binary` alone sends `application/x-www-form-urlencoded`, and `curl -F` sends a form: give the type, or
`X-PureByte: 1`, as above.

### Errors

Errors are JSON, `{"error": {"code": "...", "message": "..."}}`, with these HTTP statuses:

| Status | Codes |
|---|---|
| 400 | `empty_input`, `no_input` (a JSON or form body without an input), `bad_json`, `bad_parameter` (a parameter the endpoint does not take, a bad value, or `/v1/redact` with a model that has no typed spans), `bad_request` |
| 401 | `unauthorized` (a token is set and the request lacks it) |
| 403 | `bad_host`, `bad_origin` (a request from a web page of another site), `missing_header` (a form, plain-text or untyped body without `X-PureByte` or the token); see [Security](#security) |
| 404 | `unknown_model` (the message lists the loaded ones), `not_found` |
| 405 | `method_not_allowed` (the endpoints that take an input accept only `POST`, and `/health` and `/v1/models` only `GET`; the `Allow` header says which) |
| 413 | `too_large_for_decide` (over 4,096 bytes on `/v1/decide`), `too_large` (over the model's limit, on `/v1/scan` and `/v1/redact`), `payload_too_large` (over `--max-body-mb`) |
| 500 | `engine_failure` (the details go to the server's standard error, never to the client) |

### Limits

| Limit | Default |
|---|---|
| Input to `/v1/decide` | 4,096 bytes; a body announced over 64 KiB is refused before it is read, and a chunked one is not read past 64 KiB |
| Input to `/v1/scan` and `/v1/redact`, `secrets-code` profile | 4,000,000 bytes |
| Input to `/v1/scan` and `/v1/redact`, other profiles | 64 MiB |
| Request body | 80 MiB (`--max-body-mb`) |
| Reading and writing a request | 60 seconds without progress, each: the time between two reads or two writes, not a total (a client that keeps sending slowly keeps one of the `--http-threads`) |
| Archives | Scanned as they are; with `--archives`, their members within the [archive limits](cli.md#scan) |
| Inputs shorter than the model's shortest window, `window.min` (24 bytes unless the model declares another; the released `pii` declares 1) | Not analyzed, and the result says so in its `note`; `/v1/redact` analyzes inputs of any length |

### Security

- **Loopback by default.** The server binds to `127.0.0.1`. Use `--host` to bind elsewhere, and then set a token; the
  server warns when it listens on anything but loopback.
- **DNS rebinding protection.** A request whose `Host` header is not `127.0.0.1`, `localhost`, `[::1]`, the address
  given with `--host` or a name given with `--allow-host` is rejected with `bad_host`, so a malicious web page cannot
  reach the server through a domain of its own that resolves to your machine. The check applies on loopback, and on
  any address when there is no token (as in a container that listens on `0.0.0.0` behind a port published on
  `127.0.0.1`).
- **Web pages of other sites cannot use it.** Browsers name the page behind a request in `Origin`: a request whose
  `Origin` is not a page of this machine (http or https, `127.0.0.1`, `localhost` or `[::1]`, any port) is refused with
  `bad_origin`, `Origin: null` included. A page can still send a form or plain text to any address without asking the
  browser (and old browsers leave `Origin` out), so such a POST, or one with no type, needs `X-PureByte` or the token:
  no page can add either without the server's consent, and the server never gives it (it approves no CORS
  preflight). Programs such as curl send no `Origin`; they only give a type or the header, as in the examples above.
- **Token.** With `--token TOKEN`, `--token-file FILE` (its first line) or `PUREBYTE_SERVE_TOKEN`, every endpoint
  except `/health` requires `Authorization: Bearer TOKEN` (the scheme in any case); the comparison takes the same time
  wherever the strings differ. A token given empty is refused at start-up, so that an unset variable in
  `--token "$TOKEN"` never starts a server without one, and the start-up line says whether a token is required.
  Prefer the file or the variable: other users of the machine can read a command line, and the server warns when the
  token is on it.
- **The port is the server's alone.** No other program can bind it while the server runs (`SO_EXCLUSIVEADDRUSE` on
  Windows; elsewhere the port is not shared either).
- **Bounded work per request.** Inference runs one request at a time and cannot be interrupted: archives are not
  expanded unless the server was started with `--archives`, and `/v1/scan`, `/v1/decide` and `/v1/redact` refuse an
  input over the model's limit before any inference, so the work of a request is bounded by its size and the model's
  input limit. A body costs about its own size in memory: it is never parsed as a form.
- **No disk writes, no content in logs.** Bodies and results stay in memory; the access log (`--quiet` turns it off)
  records method, path, status and sizes only, with control characters escaped so that a request cannot write a line
  of its own.
- **No paths.** The server never opens a path it receives; it only reads the bytes you send.

## C API

The library is `libpurebyte` (`purebyte.dll`, `libpurebyte.so` or `libpurebyte.dylib`, or the static
`purebyte_static`), and its interface is one header, [`include/purebyte/pb.h`](../engine/include/purebyte/pb.h), which
documents every function. The conventions:

- Every fallible function returns a `pb_status` (`PB_OK` is 0) and fills an optional `pb_error` with a message. The
  library never exits the process.
- A loaded `pb_model` is immutable and can be shared by threads. A `pb_session` owns worker threads and scratch
  memory: one per thread, or serialize the calls.
- Offsets and lengths are in bytes; spans are half-open `[start, end)`.
- Memory returned by the library is released with the matching `*_free` function (`pb_free` for strings and
  buffers).

**ABI rule.** Option structs start with `struct_size`: always initialize them with the matching `*_init` function
(`pb_session_options_init`, `pb_scan_options_init`, `pb_detect_options_init`, `pb_redact_options_init`,
`pb_archive_limits_init`). Those functions are `static inline` in the header, so they are compiled into your program
and record the size of the struct **your program** was built with. Structs only grow by appending fields: a newer
library accepts the smaller struct of an older program (the fields it lacks keep their defaults), and an older
library refuses the larger struct of a newer program with `PB_ERR_UNSUPPORTED` rather than ignore fields it cannot
honor. `PB_ABI_VERSION` changes on any other incompatible change; `pb_abi_version()` returns the value the loaded
library was built with, and `pb_version()` its version.

**Build and link.** Compile against the header and link the library. With the shared library, define
`PUREBYTE_SHARED` (the CMake target `purebyte` does it for you; on Windows it selects the DLL import declarations):

```bash
cc -std=c99 -DPUREBYTE_SHARED -I/usr/local/include example.c -L/usr/local/lib -lpurebyte -o example
```

The static library is C++ inside: link it with a C++ compiler, or add its C++ runtime.

Detect credentials in a buffer and print the JSON result:

```c
#include <stdio.h>
#include <string.h>
#include <purebyte/pb.h>

int main(void) {
    pb_error err;
    pb_model *model = NULL;
    if (pb_model_load("purebyte-secrets-code-1.0.0.gguf", &model, &err) != PB_OK) {
        fprintf(stderr, "load: %s\n", err.message);
        return 2;
    }

    pb_session_options session_options;
    pb_session_options_init(&session_options);
    session_options.threads = 4;
    pb_session *session = NULL;
    if (pb_session_create(&session_options, &session, &err) != PB_OK) {
        fprintf(stderr, "session: %s\n", err.message);
        pb_model_free(model);
        return 2;
    }

    const pb_model *members[] = {model};
    pb_detector *detector = NULL;                            /* NULL below: the profile the model declares */
    if (pb_detector_create(members, 1, NULL, &detector, &err) != PB_OK) {
        fprintf(stderr, "detector: %s\n", err.message);
        pb_session_free(session);
        pb_model_free(model);
        return 2;
    }

    const char *text = "DB_PASSWORD = \"Tr0ub4dor&3xyz\"\n";
    pb_input input = {(const uint8_t *)text, strlen(text)};
    const char *names[] = {"settings.py"};
    pb_detect_options options;
    pb_detect_options_init(&options);

    char *json = NULL;
    if (pb_detect(session, detector, &input, 1, names, &options, &json, &err) == PB_OK) {
        puts(json);                                            /* values are masked unless PB_DETECT_REVEAL */
        pb_free(json);
    } else {
        fprintf(stderr, "detect: %s\n", err.message);
    }

    pb_detector_free(detector);
    pb_session_free(session);
    pb_model_free(model);
    return 0;
}
```

An ensemble is several models in `members` (the first is the main one); `options.flags` takes `PB_DETECT_*` values
such as `PB_DETECT_REVEAL`, `PB_DETECT_PER_MODEL` or `PB_DETECT_EXPAND_ARCHIVES`. Every flag is off unless you set it:
with the `secrets-binary` profile, `PB_DETECT_STRINGS_ONLY` keeps only the findings that touch a printable string,
which the CLI, the HTTP API and the Python package do by default.

Every finding has a `severity`, `"error"` or `"warning"`, from the path of its document: the `secrets-code` profile
gives warnings to test, example and documentation paths ([spec/OUTPUT.md](../spec/OUTPUT.md#51-severity)). The path
is the input's name unless `options.paths` (one entry per input, `NULL` entries allowed) gives its path inside the
project, which matters when names start above the project: a program that scans `/home/me/test/repo` passes
`src/app.py`, not `/home/me/test/repo/src/app.py`, whose `test/` would make it a warning.

Other calls:

| Call | What it does |
|---|---|
| `pb_scan` | Runs a model over windows and returns, per window, the values of every head and the decoded spans; it also lets you override the window, the stride and the prefilter rule |
| `pb_encode` | The final hidden states of an input run as one sequence |
| `pb_model_describe` | What a model file declares, as JSON |
| `pb_detector_wants_file`, `pb_detector_wants_directory`, `pb_detector_max_input_bytes` | What the detector's profile reads, for programs that walk directories |
| `pb_detector_path_severity` | The severity (`PB_SEVERITY_ERROR` or `PB_SEVERITY_WARNING`) the profile gives to the findings of a file, from its path inside the project; for programs that skip test paths, as `scan --tests no` does |
| `pb_redact`, `pb_redact_spans`, `pb_restore` | Redaction by copy with a model, or of spans that come from anywhere, and its reversal |
| `pb_archive_expand` | The members of a zip, gzip or tar archive, recursively, within limits against archive bombs |

## Python

```bash
pip install purebyte
```

```python
import purebyte

result = purebyte.scan("settings.py", model="secrets-code")
for finding in result.findings:
    print(finding.file, finding.line, finding.col, finding.snippet_masked)
```

The package wraps the same C library through `ctypes` (standard library only) and returns the same results. It also
installs the `purebyte` command, which runs the native CLI of the package.

| Function or class | What it does |
|---|---|
| `scan(sources, model="secrets-code", *, profile=None, threads=None, detector=None, session=None, exclude=(), **options)` | Scans files, directories, `bytes`, or a list of them; returns a `ScanResult`. Directories are walked as the CLI walks them: the profile's file and folder rules, links and junctions not followed, and the paths that match `exclude` (the CLI's `--exclude`) or the folder's `.purebyteignore` left out. Files are read in batches of about 64 MiB, and an input the library fails on is retried alone and listed in `failures` |
| `decide(data, model="secrets-code", ...)` | One input of up to 4,096 bytes; returns `{"decision": "positive" or "negative", "result": ..., "findings": [...]}` |
| `redact(data, model, *, types=None, with_map=False, ...)` | A `Redaction`: `output` (bytes), `report` (never the values) and, with `with_map=True`, `map` (the values: a secret) |
| `redact_spans(data, spans, type_order=(), with_map=False)` | Redaction of spans that come from anywhere: `[(start, end, type, confidence, source), ...]` |
| `restore(redacted, redaction_map)` | The original bytes from a redacted copy and its map |
| `expand_archive(data, name="", max_depth=None)` | The members of an archive, and what could not be extracted |
| `load_detector(model="secrets-code", profile=None, ensemble=True, extra_members=())` | A `Detector` for an installed specialist or a model file, resolved as the CLI's `--model`: a string that ends in `.gguf`, starts with `.` or has a folder in it, or an `os.PathLike`, is a file; any other string is a name of the catalog |
| `Model(path)`, `Session(threads=None, kernel="auto", intra_threads=0)`, `Detector(models, profile=None)` | The building blocks, to load once and reuse across calls; `Detector.path_severity(path)` is `"error"` or `"warning"` |
| `models_directory()`, `version()` | Where models are installed; the version of the loaded library |

`options` are those of `Detector.detect`: `reveal`, `per_model`, `ensemble`, `votes`, `bias`, `type_bias`,
`min_confidence`, `archives`, `prefilter`, `strings_only` (`True` by default: with `secrets-binary`, only the findings
that touch a printable string; `False` is the CLI's `--all-bytes`), `early_exit`, `keep_examples`, `queries` and
`paths` (each input's path inside the project, for the severity; `scan` computes them as the CLI does). A
`ScanResult` has `findings` (objects whose attributes are the fields of a finding, `severity` included; the ones a
finding lacks are `None`), `by_severity` (`{"error": n, "warning": n}`: only errors should fail a check), `results`
(the per-input results as dictionaries) and `failures` (`{"file", "reason"}`); file names use `/` as the separator on
every platform, as the CLI writes them. Errors raise `PureByteError`, whose `status` names the `pb_status`; the
errors found by the package itself are also instances of the built-in exception that describes them: `LookupError`
for a model name the catalog does not know, `FileNotFoundError` for a specialist that is not installed, `ValueError`
for a checksum that differs or an argument out of range (an input over a limit, for example). Every function has a
docstring: `help(purebyte)`.

For repeated calls, build the detector and the session once:

```python
detector = purebyte.load_detector("secrets-code")
session = purebyte.Session(threads=4)
for line in lines:
    result = purebyte.scan(line.encode(), detector=detector, session=session)
```

The package reads the same environment as the CLI (`PUREBYTE_HOME`, `PUREBYTE_CATALOG`), plus `PUREBYTE_LIBRARY` (the
library file to load instead of the one inside the package) and `PUREBYTE_CLI` (the executable the `purebyte`
command runs).
