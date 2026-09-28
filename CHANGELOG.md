# Changelog

All notable changes to the PureByte runtime are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html). Specialists have their own versions, recorded in their
[model cards](models/README.md).

## [Unreleased]

## [1.0.0] - 2026-09-28

First public release. The training stack that makes the AI Specialists lives in its own repository,
with its own changelog: [purebyte-ai/purebyte-train](https://github.com/purebyte-ai/purebyte-train).

### Added

- `libpurebyte`, a C++17 runtime for byte-level models with no third-party dependencies: declarative model files
  (GGUF), registries of blocks, heads and post-processing profiles, windowed and streaming execution, and scalar,
  AVX2 and NEON kernels with bit-identical results on each platform.
- The `purebyte` CLI: `scan`, `decide`, `redact`, `serve`, `models`, `bench`, `info` and `version`; JSON, JSON Lines
  and SARIF 2.1.0 output; git diffs, standard input and archives as inputs.
- A severity on every finding: `secrets-code` findings in test, example and documentation paths are warnings, which
  are reported but do not fail `scan` (exit status 1 on errors only; `--strict` counts warnings, `--tests no` skips
  those paths).
- Redaction by copy for any model with typed spans, with a report that never holds the values and an optional map to
  restore the original. Every input is analyzed, whatever its length; with `secrets-code`, a UTF-16 input is redacted
  in UTF-16, with `"encoding"` in the report and the map; the report's `input` names the file (`pb_redact_options.name`,
  the CLI's input, the API's `filename`). Distinct names in any script, and distinct phone numbers, never share a
  marker; the case and accents of Latin, Greek and Cyrillic names are folded. Numbers and JSON do not depend on the
  process locale.
- Threads that share the work of one window, for low latency on single small inputs.
- A local HTTP API (`purebyte serve`) with health, model listing, scan, decision and redaction endpoints.
- Python bindings (`pip install purebyte`).
- The model format and output specifications, and a NumPy reference implementation of the forward pass.
- Integrations: pre-commit hook, GitHub Action with code scanning upload, GitLab CI template, Docker image, VS Code
  tasks.
- Release artifacts built and tested by one workflow on each `vX.Y.Z` tag ([docs/releasing.md](docs/releasing.md)):
  archives of the CLI with SHA-256 files for Linux and macOS (x86-64 and arm64) and Windows (x86-64), which need no C
  or C++ runtime from the system (nor does the Windows DLL of the wheel), wheels for the same platforms, and a Docker
  image for amd64 and arm64.
- A reproducible CredData evaluation of the `secrets-code` specialist, with an option to leave out the repositories
  that overlap a training corpus.
- `pii` 1.0.0 in the model catalog (`purebyte models pull pii`), and a reproducible evaluation of it on the PII
  Masking Benchmark with the leaderboard's metric (`benchmarks/piimb/`), also without the benchmark sentences that
  share content with its training data.
- The script that draws every chart of the README from the published numbers (`docs/assets/make_charts.py`).
- A latency comparison with the Laya decision model on the same machine ([docs/performance.md](docs/performance.md#compared-with-laya-and-jev)).
- Ways to accept a finding, as other secret scanners have: with `secrets-code`, a finding that starts on a line
  carrying `purebyte:allow` (in a comment) is not reported; `scan --exclude GLOB` and a `.purebyteignore` file at the
  top of a scanned folder (or of the repository, with `--staged` and `--git-diff`) leave paths out with
  `.gitignore`-style patterns.
- `secrets-binary`: spans in the metadata of signed JARs (the `SHA-256-Digest: ...` digests and the `Name: ...` entry
  paths of manifests and signature files, continuation lines included) are dropped: they are hashes and paths, never
  credentials. On the 69 binary files in 14 formats of the bin-fp-c exam, with their containers opened, the JAR
  metadata gave 373 findings before the rule and none after; 4 findings remain, in two class files (7 with
  `--all-bytes`).
- `secrets-binary`: by default in the CLI, the HTTP API and the Python package, a finding must touch a printable string
  (16 or more ASCII or UTF-16LE characters); `scan --all-bytes` and `decide --all-bytes`, `strings_only=no` and
  `strings_only=False` report the others. The C API keeps `PB_DETECT_STRINGS_ONLY` opt-in.
- CMake 3.16 or newer with GCC, Clang or MSVC 19.14 (Visual Studio 2017 version 15.7) or newer; floating-point
  contraction is off with every compiler, clang-cl included; big-endian targets are refused, since model files are
  little-endian; `cmake --install` exports a `purebyte` CMake package.
- When the next window would be shorter than `window.min`, one last full window aligned to the end of the input covers
  the remaining bytes (engine and reference); the released models' size and stride never need it.

### Security

- Masked context lines and strings never show part of a detected token: a finding that covers only part of a token
  (a JWT, a base64 key) is masked together with the whole token, while the name in `key=value` stays readable. Offsets
  and `snippet_masked` are not affected.
- Hostile inputs cost bounded work and memory: model files whose tensors overlap, or whose n-gram tables exceed 2^24
  buckets or 65,536 columns, are refused; an archive counts every byte it decompresses (kept or refused) and every
  name it reports against its budget, refuses zip entries that share data, cuts member names beyond 4 KiB, lists at
  most 1000 failures and reports a malformed gzip header as a failure; `max_depth` is capped at 16 and the ratio limit
  cannot overflow; overlapping findings are masked as one; on a long line, a finding's context is the part of the
  line within 256 bytes of it.
- The CLI runs `git` and `curl` from the absolute folders of `PATH` only, never from the current folder or an empty,
  `.` or relative `PATH` entry: a repository that ships a `git.exe` or `curl.exe` at its root cannot have it run by
  `scan --staged`, `scan --git-diff` or `models pull`.
- `scan --staged` and `--git-diff` take file names literally, never as git patterns or revisions (`[ab].py`,
  `0:app.py`), and `--staged` reads each file by its object id; walks do not follow junctions (Windows), and
  `--git-diff` does not follow a link of the working tree.
- `scan` retries a batch that the runtime fails on input by input: an input that still fails is reported as not
  scanned, and the report of the others is still written.
- `serve` refuses requests from web pages of other sites (`Origin`) and form, plain-text or untyped POSTs without the
  `X-PureByte` header or the token; expands archives only with `--archives`; keeps its port to itself
  (`SO_EXCLUSIVEADDRUSE` on Windows, no `SO_REUSEPORT` elsewhere); and also takes its token from `--token-file` or
  `PUREBYTE_SERVE_TOKEN`, since other users of the machine can read `--token` on its command line.
- Masked views (`string_masked`, `context_masked`, the per-model views of an ensemble, SARIF and the HTTP API) hide
  every span any model found in them, including spans a profile rule dropped or folded and every ensemble member's
  findings; a PEM header stays readable only when its label is made of key-type words. Findings, votes and exit
  statuses do not depend on masking.
- Model files are refused at load (`PB_ERR_FORMAT`) when they hold NaN or infinite values (tensors or metadata),
  integers out of range, a second tag head, more than 256 entity types, a stride or `window.min` larger than a
  window's document bytes, or windows whose cost exceeds the limits of the format; a value that overflows at run time
  never counts as positive. `pb_scan` refuses windows or strides above 2^30 and windows with fewer document bytes than
  `window.min`; `pb_archive_expand` refuses a negative `max_depth`.
- `pb_session_create` fails with `PB_ERR_NO_MEMORY` when a worker thread cannot start, instead of ending the process;
  error messages escape control characters and never carry invalid UTF-8.
