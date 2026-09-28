# AGENTS.md

Instructions for AI coding agents (and for humans who like checklists) working in this repository. Read this file
before changing anything. [CONTRIBUTING.md](CONTRIBUTING.md) has the same rules in a shorter form.

## What this repository is

PureByte is an architecture for tiny byte-level models, called **AI Specialists**, that each do one thing very well.
This repository holds the open runtime that runs them (a C++17 engine that runs any model declared by its GGUF file,
the `purebyte` CLI and local HTTP server, thin Python bindings, the model format and output specifications, a NumPy
reference implementation), the integrations, the documentation, and the evaluations of the three example specialists
of the first release (`pii`, `secrets-code`, `secrets-bin`), reproducible on public data except the binary exams of
`secrets-bin`. The engine core and the model
format are task-agnostic; what is specific to a task lives in the model file and in the post-processing profile it
selects (`none`, `redact`, and the `secrets-code` and `secrets-binary` profiles of the examples), so a new specialist
never needs a change to the core.

The training stack that makes those models (the `pbtrain` package, the specialists' recipes and data generators, and
the scripts that build their data from pinned public sources) lives in its own repository,
[purebyte-ai/purebyte-train](https://github.com/purebyte-ai/purebyte-train), with its own AGENTS.md: see [Training](#training). No dataset file and no model
weight is ever committed here: weights are released separately (see [Out of scope](#out-of-scope)).

## Map

| Path | Contents | Notes |
|---|---|---|
| `engine/include/purebyte/pb.h` | The C API | The only stable interface; ABI rules below |
| `engine/src/` | The library: `core`, `format`, `kernels`, `model`, `blocks`, `heads`, `runtime`, `detect`, `api` | See [docs/architecture.md](docs/architecture.md) |
| `cli/` | The `purebyte` binary: `scan`, `decide`, `redact`, `serve`, `models`, `bench`, `info`, `version` | Runs models through the C API only; shares the engine's core utilities (JSON, SHA-256, files, UTF-8) |
| `python/` | `ctypes` bindings and the same CLI entry point | Standard library only |
| `spec/` | `FORMAT.md` (model files, numerics contract) and `OUTPUT.md` (results, SARIF) | Normative: code follows the spec |
| `reference/` | NumPy forward pass | Must match the engine on every component type |
| `tests/` | Unit, golden and exactness tests; random tiny models generated from the spec | Golden files lock behavior |
| `benchmarks/` | Published results and the scripts to reproduce them on public data | Numbers only with conditions |
| `models/` | Model cards and `models.json` (catalog: URLs and checksums) | No weights in git |
| `integrations/` | pre-commit, GitHub Action, GitLab CI, Docker, VS Code | Each folder has a README |
| `docs/` | User and contributor documentation | One topic per page, linked from the README |
| `.pre-commit-hooks.yaml` | The published pre-commit hook | Must stay at the repository root |

## Build and test

Requirements: CMake 3.20 or newer, a C++17 compiler (GCC, Clang, MSVC or MinGW-w64), Python 3.8 or newer.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build --build-config Release --output-on-failure
```

- CMake options: `PUREBYTE_BUILD_CLI`, `PUREBYTE_BUILD_TESTS` and `PUREBYTE_BUILD_SHARED` (all on by default), and
  `PUREBYTE_STATIC_RUNTIME` (off by default; release binaries turn it on).
- The test suite includes the golden and exactness tests. They must pass on every change to `engine/`, `cli/`,
  `python/`, `spec/` or `reference/`.
- CI builds and tests on Linux (GCC and Clang), macOS (arm64 and x86-64) and Windows (MSVC and MinGW-w64). Code that
  only works on one platform is a bug.
- The compiler flags that protect the numerics (`-O2`, `-ffp-contract=off`, `-fno-fast-math`, `/fp:precise`) are part
  of the contract: never change them to gain speed.
- Scratch and build directories go outside the source tree or under `build/`; never commit them.

## Invariants

Breaking one of these is a blocking defect, whatever else the change achieves.

### Exactness and determinism

1. **The numerics contract** in [spec/FORMAT.md](spec/FORMAT.md) is law. On one platform, the scalar, AVX2 and NEON
   kernels produce bit-identical results; the scalar kernel is the executable specification of the order of
   operations. A faster kernel that changes one bit of one result is not an optimization, it is a different model.
2. **Same results at any thread count**, for batches and within a window.
3. **Golden files are never regenerated to make a test pass.** When a change is meant to alter results, the pull
   request says which results change, why, and shows the before and after; then, and only then, the goldens are
   regenerated in the same change.
4. **Across operating systems**, raw values may differ slightly, because math libraries differ and the layers amplify
   their last-place differences (the tests allow a relative difference of 1e-4 on head values). Decisions and spans
   must stay identical on the test suite's inputs, and are expected to agree on any input
   ([spec/FORMAT.md](spec/FORMAT.md) §5); never claim more than that.
5. **Every opt-in approximation is labeled** (today: the string prefilter and early exit) and is off by default.

### Privacy and security

1. **Never print, log or store a detected value in clear** unless the caller explicitly asked to reveal it. This
   covers error messages, debug output, test logs, assertions, SARIF and exceptions. Use the masking function; never
   write your own.
2. **No network access** in the runtime except `purebyte models pull`, and in the training stack except the data
   scripts' downloads of their pinned sources. No telemetry, no update checks, no crash reporting.
3. **Never verify a credential** against any service, not even in tests.
4. **Tests use generated look-alike values**, preferably assembled at test time (for example from parts), so that the
   repository holds no literal that could pass for a live credential. Never paste a real secret into code, tests,
   fixtures, documentation, commit messages or issues, and never copy one from an input you were asked to scan.
5. **Every input is untrusted**, model files included: bounds-check everything, refuse what is not understood, keep
   the archive limits (depth, size, compression ratio), and never follow paths that come from an input or a request.
6. **The HTTP server** stays on loopback by default, keeps its `Host` check and optional token, writes nothing to disk
   and logs no content.

### Interfaces

1. **C API**: every fallible function returns a `pb_status` and never exits the process; option structs start with
   `struct_size` and new fields are only appended; `PB_ABI_VERSION` changes on any incompatible change.
2. **Model format**: a file that uses something the runtime does not implement is refused with a message that names
   it; nothing is silently ignored or guessed. Format changes are versioned in `spec/FORMAT.md`.
3. **CLI**: exit status 0 means no finding of severity `error` (warnings, such as secrets-code's findings in test,
   example and documentation paths, count only with `--strict`), 1 means findings, 2 means the scan could not
   complete. Output schemas follow `spec/OUTPUT.md`; additions are backward compatible.

### Dependencies and language

1. No third-party dependencies in the engine or the CLI (the single-header HTTP library is vendored). The Python
   package uses the standard library only.
2. English everywhere: identifiers, comments, messages, documentation.

## Style

- **C++17.** No exceptions across the C API, no global mutable state, RAII for ownership, fixed-width integer types
  in data formats. Keep hot loops free of allocation. Follow the formatting of the file you edit.
- **Errors** say what happened and what to do next, for example: "head 2 has type `foo`, which this runtime does not
  implement (known: choice, multilabel, ...). Update purebyte or use a model built for this version."
- **Python.** Standard library only, type hints on public functions, no network access in tests.
- **Docs.** American English, short sentences, sentence-case headings, one topic per page, links instead of
  repetition, conditions next to every number, TBD for what is not measured and *provisional* for what can still
  change. The product terms are written **AI Specialists** (the category of models) and **Vibe Training** (training
  one by describing it to an AI coding agent), always with these capitals.

## Extending the runtime

Each extension point is a module that implements an interface plus one line in a registry. The core does not change.

**A block type** (`engine/src/blocks/`)

1. Implement `Block` (see `blocks/block.h`): scratch sizes, `forward`, `describe`, and `stream_state_floats` if it can
   stream.
2. Add one line to `blocks/registry.cpp`.
3. Specify its metadata keys, tensors and numerics in `spec/FORMAT.md`, with the format version that introduces it.
4. Implement the same function in `reference/` and add golden tests on random models generated from the spec.

**A head type** (`engine/src/heads/`): the same steps with `Head` (`heads/head.h`) and `heads/registry.cpp`, plus its
representation in the generic output (`spec/OUTPUT.md`).

**A profile** (`engine/src/detect/`): implement the profile interface, register it, document its output in
`spec/OUTPUT.md`, and add end-to-end tests with look-alike inputs. A profile must keep the masking rules.

**A kernel** (`engine/src/kernels/`): implement the kernel table for the new instruction set, add run-time dispatch,
and prove bit-identity with the scalar kernel on the exactness tests. Reproduce the scalar order of operations exactly
(fused multiply-add lanes, summation tree); do not let the compiler contract or reassociate.

**A CLI subcommand or option** (`cli/`): run models through the C API only (the core utilities of `engine/src/core`
may be shared), keep the exit status convention, add the option to
`purebyte <command> --help` and to [docs/cli.md](docs/cli.md), and add an end-to-end test.

**An input format** (archives, encodings): add it to the detect layer's input expansion with explicit limits, report
members as `outer!/inner`, and add tests with crafted hostile inputs (bombs, deep nesting, path traversal).

**An integration** (`integrations/`): a folder with a README, pinned versions, checksum verification for anything
downloaded, and exit status handling that distinguishes findings (1) from failures (2 or more).

**A benchmark** (`benchmarks/`): public data only, the split fixed before measuring and computable by anyone, scoring
code that never prints a value, other tools run in the same harness where they can be run (and quoted with their
source where they cannot, as the PIIMB leaderboard is), every pre-registered verdict published with its failures, and
results with their conditions in `benchmarks/RESULTS.md`.

## Training

The models this runtime executes are **AI Specialists** (tiny models that do one thing very well, on a CPU, privately
and measurably). They are made with the training stack in [purebyte-ai/purebyte-train](https://github.com/purebyte-ai/purebyte-train): the `pbtrain` package,
the specialists' recipes and generators, and the data scripts. When a user asks you to train one, that is **Vibe
Training**: work in that repository and follow the golden path of its AGENTS.md. Two rules tie the repositories
together:

- A block or head type is added to the runtime first (spec, engine, NumPy reference, goldens), then to the trainer.
- Every file the trainer exports must load in this engine and give the same spans as PyTorch: the training repository's
  tests build `tests/model_dump` from this repository (`PUREBYTE_MODEL_DUMP`) to check it.
- The training repository keeps exact copies of `reference/purebyte_ref/` and `benchmarks/creddata/score.py` and
  `benchmarks/piimb/score.py`, and its CI compares them with `main` here byte for byte: change them here first, then
  refresh the copies there in the same week.

## Out of scope

- Dataset files, downloaded or generated (they go to `$PUREBYTE_DATA`; only the scripts and manifests that build
  them are in git), run artifacts and logs.
- Model weights of any kind (they are released separately; see [models/](models/README.md)).
- Telemetry, remote logging, online verification of credentials, calls to any online service. The only network
  access is `purebyte models pull` and the data scripts' downloads of their pinned public sources.
- GPU backends in the runtime. The runtime targets CPUs (training may use a GPU: see [purebyte-train](https://github.com/purebyte-ai/purebyte-train)).
- Internal names of the maintainers' private tooling, experiments or model versions.

If a task seems to need one of these, stop and ask the maintainers instead.

## Before you finish

- [ ] Built and tested as in [Build and test](#build-and-test), and the relevant part of the suite ran.
- [ ] No result changed, or the change is explained and the goldens were regenerated on purpose.
- [ ] No detected value can reach any output unmasked.
- [ ] No new dependency, no new network access.
- [ ] Documentation updated where users will look for it: the README for what every user needs, `docs/` for the
  details, `benchmarks/RESULTS.md` for results; charts redrawn with `docs/assets/make_charts.py` when a number
  changes.
- [ ] `CHANGELOG.md` updated for user-visible changes.
- [ ] No real secret, personal data, internal path or private name in the diff.
