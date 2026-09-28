# Tests

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build --build-config Release --output-on-failure
```

The C++ unit tests need nothing else. The model-based tests need Python 3.8 or newer (standard library only), and the
comparison with the reference implementation also needs NumPy; without it that test is reported as skipped.

## What runs

| ctest name | What it checks |
|---|---|
| `models` | Writes the random tiny models of [gen/random_models.py](gen/random_models.py) into `build/tests/work/models` (a setup step for the others) |
| `unit` | C++ unit tests ([cpp/](cpp)): every SIMD kernel against the scalar kernel bit for bit, the correctly rounded FMA, team shares and barriers, window enumeration, the query template, BIOES decoding against an exhaustive search, the stream context against one forward pass, the C API contract, redaction properties |
| `detect` | The detect layer through `pb_detect`: every option combination on the random models, masking |
| `reference` | The engine against the NumPy reference ([reference/](../reference)) on every random model and scan variant |
| `golden` | The engine against the golden files in [golden/](golden) |
| `invariance` | Every kernel (scalar and the SIMD kernel of the CPU) and six thread arrangements give the same bits |
| `validation` | 52 malformed or contradictory model files are refused with the right status and a message that names the problem; the reference reader refuses the 15 broken containers among them too, and both read a container whose strings are not UTF-8 |
| `cli` | End-to-end tests of the `purebyte` binary: exit statuses, JSON, JSON Lines and SARIF reports, standard input, masking, the `secrets-binary` default (`--all-bytes`), the retry of a failed batch input by input (the test-only variable `PUREBYTE_TEST_FAIL_INPUT=NAME` makes the engine calls that include `NAME` fail), `git` and `curl` taken from `PATH` only (never the current folder), literal file names with `--staged` (with git, else skipped), junctions not followed (Windows), and the local server's defenses: `Origin`, `X-PureByte`, token file and variable, exclusive port, archives off by default |
| `python` | The Python bindings ([python/tests](../python/tests)) on this build's library and CLI: scans, archives, redaction, the HTTP server, model resolution |

Run one C++ test with `build/tests/purebyte_tests NAME-SUBSTRING` (or `-SUBSTRING` to run all but those), and one
Python test directly, for example:

```bash
python tests/python/test_reference.py --model-dump build/tests/model_dump --work build/tests/work --model stream-context
```

## The random models

[gen/random_models.py](gen/random_models.py) writes seven small models directly from
[spec/FORMAT.md](../spec/FORMAT.md). Their weights come from a SplitMix64 generator seeded by the model's name, so the
files are byte-identical on every platform. Together they cover every block type and direction, every head type and
pooling, 32-bit and 4-bit n-gram tables with one or two hash heads, the context gate and the injection layer, the
lookahead, the query template, the stream context, gating, early exit, per-block hyper-parameters, ternary and float
projections, and sizes that are not multiples of the 8 lanes of the numerics contract. A new block or head type
comes with a model here.

[tools/model_dump.cpp](tools/model_dump.cpp) prints everything the engine computes for a model over the inputs of a
model (window geometry, flags, every head's outputs, digests, optionally the hidden states) as JSON; the reference
prints the same JSON (`python -m purebyte_ref`).

## Comparison rules

The numerics contract ([spec/FORMAT.md](../spec/FORMAT.md), section 5) promises the same bits from every kernel and
thread count on one platform, and results that differ across platforms only through the C library's `expf`, `logf`,
`log1pf`, `sinf` and `cosf` (a few units in the last place, amplified by the layers). Hence:

- **Same platform, same library:** identical bits (the `invariance` test; the digests of the `golden` test when the
  platform and its C library fingerprint match the goldens').
- **Across implementations or platforms** (`reference`, `golden` elsewhere): window geometry, flags, labels, levels,
  spans and byte maps must be identical; head values (probabilities, scores, span confidences) must satisfy
  `|a - b| <= 1e-5 + 1e-4 |b|` and final hidden states `|a - b| <= 1e-4 + 1e-3 |b|`. The observed differences use about
  1.5% of these tolerances.

## Golden files

`tests/golden/<model>.json` hold what the engine computed for each random model and locked variant, the SHA-256 of the
model and input files they were made from, and the platform (with a fingerprint of its C library). They are never
regenerated to make a test pass. When a change is meant to alter results, say which and why in the pull request,
then regenerate them in the same change:

```bash
python tests/python/test_golden.py --model-dump build/tests/model_dump --work build/tests/work --update
```

## Local tools

[local/](local) holds tools that need things outside this repository and never run in CI:
`check_exactness.py` compares the engine with a reference engine's window reports on released models, and
`neon_on_x86.sh` validates the NEON kernel on an x86 machine through SIMDe (the kernel tests, then every random model
with the emulated NEON kernel against the scalar kernel, bit for bit).

[tools/window_latency.cpp](tools/window_latency.cpp) measures how the latency of one window and the throughput of a
batch change with the threads (`--intra-threads`), and checks that every configuration gives the same digests:

```bash
build/tests/window_latency MODEL.gguf --iterations=50 --threads=1,2,3,4,6,8,12 --batch=48
```

Absolute times depend on the machine and its load far more than the ratios; publish both with their conditions.
