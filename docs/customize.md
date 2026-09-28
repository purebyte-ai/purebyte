# Customize: AI Specialists are files that declare what they do

The PureByte runtime knows nothing about secrets, personal data or any other task. An AI Specialist is a single model
file that **declares** its building blocks, its outputs and how those outputs become results; the runtime assembles
the model from registries of known components and runs it. The same binary runs `secrets-code`, `secrets-bin`, a
personal-data redactor or a model you train yourself.

This page explains what a model can declare and what you can change at run time. The exact metadata keys, tensor
names and numerical rules are in [spec/FORMAT.md](../spec/FORMAT.md); the output schema is in
[spec/OUTPUT.md](../spec/OUTPUT.md).

## What a model declares

| Declaration | Choices | Example: secrets-code |
|---|---|---|
| Context | Windows (size and stride) or one stream with carried state | 512-byte windows every 384 bytes |
| Input template | The input as it is, or a question followed by the input | Plain |
| Input modules | Byte embedding, optionally n-gram memory tables (orders, precision, injection layer, gate) | Byte embedding only |
| Body | An ordered list of blocks from the block registry | A stack of ternary `ssm_v2` blocks |
| Heads | Any number of heads from the head registry, with label names | A `choice` head and a `tag` head gated by it |
| Aggregation | Per head, how window values combine into a document value | Spans from every window, merged by the profile |
| Operating point | Per `tag` head, a bias (one value or one per entity type) | Stored in the file |
| Profile | The post-processing module that produces the results | `secrets-code` |

`purebyte info MODEL` prints the declaration of any model file as JSON.

## The registries

These are the component types the runtime implements. A file that uses a type the runtime does not know is refused
with a message that names the type and the format version that introduced it; nothing is ever guessed.

**Input modules and blocks**

| Type | What it is |
|---|---|
| Byte embedding | One learned vector per byte value (256 rows) |
| N-gram memory | Hashed lookup tables for the last *n* bytes, one or several orders, 32-bit or 4-bit, added at the input or before a chosen layer, optionally through a learned gate |
| `ssm_v2` | Mamba-2 style selective state-space block with ternary projections; causal or bidirectional |
| `attention` | Attention block, for models that need direct access to distant bytes |
| Lookahead | A small anticausal convolution after the body, so each byte can see a few bytes ahead |

**Heads**

| Type | Produces |
|---|---|
| `choice` | One decision among N classes |
| `multilabel` | Several independent yes/no labels |
| `score` | One or more real-valued scores |
| `ordinal` | A grade on an ordered scale |
| `tag` | Typed spans (`k` entity types), decoded into consistent spans |
| `byte_map` | One label per byte |
| `exit` | An early-exit probe that lets clearly negative windows stop after a few layers |

**Profiles**

| Profile | Produces |
|---|---|
| `none` | The heads' outputs as they are, in the generic schema |
| `secrets-code` | Credential findings in text, with lines, columns, format rules, ensemble vote and masking |
| `secrets-binary` | Credential findings in binaries, with byte offsets and masked string context |
| `redact` | A redacted copy of the input, a report without values and, on request, a reversible map |

**Aggregations** of window values into a document value: `max`, `mean`, `vote`, `any`, and `min` for `score` heads.

Which component types a given release implements, and since which format version, is listed in
[spec/FORMAT.md](../spec/FORMAT.md).

## The generic output

Whatever the model, results share one schema. Per input:

| Field | Filled by | Contents |
|---|---|---|
| `decisions` | `choice` heads | Chosen class and probabilities |
| `labels` | `multilabel` heads | Labels above threshold and their probabilities |
| `scores` | `score` and `ordinal` heads | Values |
| `spans` | `tag` heads | `type`, `start`, `end` (bytes, end exclusive), `confidence`, `votes` |
| `fields` | Query-conditioned models | Extracted value locations, by field name |
| `byte_map` | `byte_map` heads | Regions of consecutive bytes with the same label |

Every span also appears in a flat list of findings (`file`, `kind`, `severity`, `start`, `end`, `confidence`,
`votes`, `snippet_masked`, and `line` and `col` for text), which profiles enrich: `secrets-code` adds the masked line
around each finding and makes findings in test, example and documentation paths warnings instead of errors,
`secrets-binary` adds the masked printable string. Every result can be written as JSON, JSON Lines (one line
per input) or SARIF 2.1.0. The authoritative definition is [spec/OUTPUT.md](../spec/OUTPUT.md).

Confidence values are the model's probabilities. They are not calibrated yet: compare them within one model, not
across models.

## What you can change at run time

No model file needs to be edited to adapt a specialist to your use:

| Setting | Effect |
|---|---|
| Profile | Use another profile, or `none` to get the raw head outputs |
| Operating point | Move a `tag` head toward precision (lower) or recall (higher), for all entity types or per type |
| Minimum confidence | Drop spans below a threshold |
| Ensemble | Run several model files together and choose how many votes a finding needs, or use only the first model |
| Queries | The fields to extract, for query-conditioned models |
| Reveal | Print detected values instead of masks (off by default) |
| Prefilter | Skip binary windows without printable strings (faster; see [performance.md](performance.md)) |
| Early exit | Let an `exit` head stop clearly negative windows (models that ship one) |
| Archives | Scan inside zip, jar, apk, gzip and tar members |
| Redaction | Write a copy of the input with every span replaced by a typed marker, with any model that has a `tag` head |
| Window and stride | Override the declared context (C API, `pb_scan` only); results then differ from the published ones, so use it for experiments only |

The corresponding CLI options are in [cli.md](cli.md), the HTTP parameters and C and Python fields in
[api.md](api.md).

## Bring your own model

Any tool that writes a GGUF file following [spec/FORMAT.md](../spec/FORMAT.md) can produce a model that this runtime
runs. To check a file:

```bash
purebyte info my-model.gguf                      # the declaration, as the runtime understood it
purebyte decide --model my-model.gguf input.bin   # run it once on a small input
```

The test suite generates small random models directly from the specification, and the
[reference implementation](../reference/) computes the same forward pass in NumPy, which makes it straightforward to
check an exporter. PureByte's own training stack, [purebyte-train](https://github.com/purebyte-ai/purebyte-train),
exports its models this way and checks that the runtime gives the same answers as the trained model. Today it trains
span-finding models (a `tag` head gated by a `choice` head); the other heads run in this runtime but are not trained
by it yet. To train a specialist, start with its
[training.md](https://github.com/purebyte-ai/purebyte-train/blob/main/docs/training.md).

## Extend the runtime

Adding a block type, a head type or a profile is one new module that implements the corresponding interface, plus
one line in its registry. The core does not change, and existing model files keep working. Every new type comes with
golden tests on random models and, for model components, a matching function in the reference implementation. The
step-by-step checklist is in [AGENTS.md](../AGENTS.md#extending-the-runtime).
