# Contributing to PureByte

Thank you for helping. PureByte makes strict guarantees (exact numerics, nothing leaves the machine, secrets are
never printed, honest evaluations), so this page is mostly about keeping them while you change things.
If you use an AI coding agent, point it to [AGENTS.md](AGENTS.md), which has the same rules in more detail.

## Ways to contribute

| Contribution | Where to start |
|---|---|
| Report a bug | The "Bug report" issue template |
| Report a false positive or a missed secret | The "False positive or false negative" template ([below](#report-a-false-positive-or-a-missed-secret)) |
| Propose a feature or a new integration | The "Feature request" template; for large changes, before writing code |
| Improve the documentation | A pull request; typo fixes need no issue |
| Add a kernel, a block, a head, a profile or an input format | [AGENTS.md](AGENTS.md#extending-the-runtime), then an issue to agree on the design |
| Improve a specialist, or add a new one | The training repository, [purebyte-ai/purebyte-train](https://github.com/purebyte-ai/purebyte-train), and its AGENTS.md; then an issue there with the task and its success criteria |
| Report a vulnerability | Privately, as described in [SECURITY.md](SECURITY.md) |

## Scope

This repository holds the open runtime (the engine, the CLI and HTTP server, the Python bindings, the model format
specification and the reference implementation), the integrations, the documentation and the evaluations of the
released models, reproducible on public data except the binary exams of `secrets-bin`. The training stack that makes the AI Specialists, their recipes and data generators,
and the scripts that build their data live in [purebyte-ai/purebyte-train](https://github.com/purebyte-ai/purebyte-train).

It never holds dataset files, model weights or training runs: pull requests that add them cannot be accepted. Data is
downloaded or generated outside the repository, and released weights are published separately under the
[PureByte Model License](MODEL_LICENSE.md).

## Set up

For the runtime you need a C++17 compiler (GCC, Clang, MSVC or MinGW-w64), CMake 3.20 or newer and Python 3.8 or
newer.

```bash
git clone https://github.com/purebyte-ai/purebyte.git
cd purebyte
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build --build-config Release --output-on-failure
```

The exact targets, test suites and options are listed in [AGENTS.md](AGENTS.md#build-and-test).

## The rules that keep PureByte trustworthy

1. **Exactness.** A change to the engine must not change any result unless that is its stated purpose. Golden tests
   lock the behavior; a golden file is regenerated only in a pull request that explains why the results change.
2. **Determinism.** Same model, same input, same results, at any thread count, on every kernel.
3. **Nothing leaves the machine.** No telemetry, no network access outside `purebyte models pull`, no verification of
   credentials against any service.
4. **Secrets are never printed.** Masking is the default everywhere, including logs, errors, test output and SARIF.
   Tests use look-alike values generated for the test, never real credentials.
5. **No new dependencies** in the engine and the CLI. The Python package uses the standard library only.
6. **English everywhere**: code, comments, messages, documentation.
7. **Honest evaluations.** Every number comes with its conditions, on public data where it exists, with the split
   fixed before measuring, and every pre-registered verdict is published, failures included. The training rules (golden fingerprints, pre-registered criteria, no leakage) are in
   [purebyte-train](https://github.com/purebyte-ai/purebyte-train).

## Style

- **C++**: C++17, no exceptions across the C API, no global mutable state, explicit error values with messages that
  say what to do. Follow the formatting of the surrounding code.
- **Python**: standard library only, type hints on public functions, no network access in tests.
- **Documentation**: American English, short sentences, sentence-case headings, one topic per page, and a link instead
  of a repetition. Every number has its conditions next to it, and a number that is not measured is written as TBD.

## Pull requests

- Keep each pull request to one purpose, with tests and documentation in the same change.
- Describe what changes for users, and paste the relevant test output.
- Commit messages: an imperative summary line (for example `engine: vectorize the gated RMS norm`), then the why.
- Sign off your commits (`git commit -s`) to certify the
  [Developer Certificate of Origin](https://developercertificate.org/). Contributions are accepted under the
  Apache-2.0 license of the repository.

## Releases

Maintainers release with a tag, and a workflow builds, tests and publishes everything. The procedure, how to verify a
release, how model weights are published and the one-time settings are in [docs/releasing.md](docs/releasing.md).

## Report a false positive or a missed secret

These reports are the most valuable feedback we get, and they must never contain a real secret.

1. Replace the value with a **look-alike**: same length, same character classes, same prefix if it has a known format
   (for example `ghp_` followed by 36 random letters and digits), but not the real value.
2. Keep the **context**: the variable name, the file type and the surrounding lines matter more than the value itself.
3. Paste the finding as the CLI printed it (masked by default) and say what you expected.
4. Tell us the specialist and CLI versions (`purebyte version`, `purebyte models list`).

If the only way to reproduce it is with real data, do not post it: describe the shape of the data instead.
