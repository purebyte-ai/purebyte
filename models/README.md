# Models

PureByte models are **AI Specialists**: tiny models that each do one thing very well, on a CPU, privately and
measurably. Each one is a single file that declares what it reads, what it answers and how its answer is
post-processed, and the same `purebyte` runtime runs all of them (see [docs/customize.md](../docs/customize.md)).
The three below are the example specialists of the first release: they show what the architecture does on three
different jobs, and any model you train runs the same way.
Weights are not stored in this repository. They are distributed through the GitHub releases of this repository,
listed with their SHA-256 checksums in [models.json](models.json), and downloaded and verified by the CLI. They are
mirrored on Hugging Face ([huggingface.co/purebyte](https://huggingface.co/purebyte)): the same files, with the same
checksums and the same license.

| Example specialist | Reads | Answers | Size | Status | Card |
|---|---|---|---|---|---|
| `secrets-code` | Source code, configuration, text (UTF-8 and UTF-16) | Where credentials are: typed spans with line, column and byte offsets | 0.98 MiB (2.9 MiB with the optional ensemble, `secrets-code-ensemble`) | 1.0.0, first release | [secrets-code.md](secrets-code.md) |
| `secrets-bin` | Any binary: executables, libraries, bytecode, WebAssembly, firmware | Where credentials are: byte offsets and the surrounding printable string | 28.8 MiB | 1.0.0, first release | [secrets-bin.md](secrets-bin.md) |
| `pii` | Any text: documents, e-mails, tickets, records, logs | Personal data masked as one type (`PII`), and a redacted copy of the input | 4.5 MiB | 1.0.0, first release | [pii.md](pii.md) |

The parameter counts of the cards are those of the network: the byte embedding, the blocks, the final norm, and the
n-gram tables with their projection. The two heads of each example add 34,439 parameters, and the per-group scales of
the ternary weights are derived, not counted.

All weights are released under the [PureByte Model License](../MODEL_LICENSE.md), which is separate from the Apache-2.0
license of the code. The model cards and [models.json](models.json) are documentation, under Apache-2.0 like the rest of
this repository.

## Get a model

```bash
purebyte models list                  # what is available, what is installed
purebyte models pull secrets-code     # download and verify the SHA-256 checksum
purebyte models verify                # re-check every installed file
```

`pull` refuses a file whose size or checksum does not match [models.json](models.json). For machines without internet
access, download the `.gguf` files listed in the catalog from the release page on a connected machine, copy them to
the models directory and run `purebyte models verify`. The install location and its override are described in
[docs/install.md](../docs/install.md#models).

## models.json

The catalog the CLI reads; it is also built into the `purebyte` binary, so `purebyte models` works offline. One entry
per specialist and version:

| Field | Meaning |
|---|---|
| `name`, `version` | Public name and semantic version of the specialist |
| `profile` | The post-processing profile to use when the model file does not declare one (`secrets-code`, `secrets-binary`, `redact`, `none`) |
| `summary` | One line: what it does |
| `reads`, `answers` | Input and output, in words |
| `card`, `license` | Relative paths to the model card and the license |
| `min_runtime` | Oldest `purebyte` version that can run it |
| `files` | The files of the specialist: `role` (`model`, or `ensemble` for extra voting members), `file`, `url`, `sha256`, `size` in bytes. Every file of an entry is downloaded by `pull` and loaded by `scan`; an optional ensemble is therefore its own entry (`secrets-code-ensemble` lists the `secrets-code` model and its two extra members) |

Fields marked `TBD` (or `null` for numbers) are filled in when the release is published; the CLI refuses to pull an
entry whose `url` or `sha256` is `TBD`. The `PUREBYTE_CATALOG` environment variable points the CLI to another catalog
file with the same schema, for example a mirror with `file://` URLs.

## Versioning

Specialists follow semantic versioning on their observable behavior:

- **Patch** (`1.0.x`): packaging or metadata only. Findings are identical.
- **Minor** (`1.x.0`): better quality with the same inputs and outputs. Findings can change; results are re-measured
  and published in [benchmarks/RESULTS.md](../benchmarks/RESULTS.md).
- **Major** (`x.0.0`): a change in what the model reads or answers.

Pin a version for reproducible CI (`purebyte models pull secrets-code@1.0.0`), and upgrade on purpose.

## Your own specialist

Everything that makes these AI Specialists is in the training repository,
[purebyte-train](https://github.com/purebyte-ai/purebyte-train): the training stack, the recipes and data generators
of each specialist, and the scripts that build their data from pinned public sources. Train your own by describing it
to an AI coding agent (**Vibe Training**), or run the steps yourself: see
[docs/training.md](https://github.com/purebyte-ai/purebyte-train/blob/main/docs/training.md). It trains on a CPU, and on
AMD Radeon GPUs (tested on Windows); NVIDIA, Linux ROCm and Apple silicon GPUs use the same stock PyTorch code but have
not been run by the maintainers yet. Today it builds span-finding specialists (a `tag` head gated by a window
decision), like the three examples. Any file that follows [spec/FORMAT.md](../spec/FORMAT.md) runs on this runtime,
whatever made it.
