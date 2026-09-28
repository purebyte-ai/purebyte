<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/logo-lockup-dark.png">
    <img src="docs/assets/logo-lockup-light.png" alt="PureByte" width="420">
  </picture>
</p>

<h3 align="center">A byte-level architecture for tiny AI models that run on ordinary CPUs.</h3>

<p align="center">
  PureByte models read raw bytes and answer with decisions, scores and exact byte ranges instead of generated text:<br>
  1 to 29 MiB, 0.8 to 1.5 ms for a short decision on a 12-core desktop CPU (3 to 6 ms on one thread), trained from
  scratch in minutes on one consumer GPU.
</p>

<p align="center">
  <a href="https://github.com/purebyte-ai/purebyte/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/purebyte-ai/purebyte/actions/workflows/ci.yml/badge.svg"></a>
  <a href="https://github.com/purebyte-ai/purebyte/releases"><img alt="Release" src="https://img.shields.io/github/v/release/purebyte-ai/purebyte?color=C6F16B&labelColor=0D0F13"></a>
  <a href="https://pypi.org/project/purebyte/"><img alt="PyPI" src="https://img.shields.io/pypi/v/purebyte?color=C6F16B&labelColor=0D0F13"></a>
  <a href="https://hub.docker.com/r/purebyte/purebyte"><img alt="Docker" src="https://img.shields.io/badge/docker-purebyte-2496ED?logo=docker&logoColor=white&labelColor=0D0F13"></a>
  <a href="https://doi.org/10.5281/zenodo.23020056"><img alt="DOI" src="https://img.shields.io/badge/DOI-10.5281%2Fzenodo.23020056-5EEAD4?labelColor=0D0F13"></a>
  <a href="https://huggingface.co/purebyte"><img alt="Hugging Face mirror" src="https://img.shields.io/badge/%F0%9F%A4%97%20Hugging%20Face-purebyte%20mirror-5EEAD4?labelColor=0D0F13"></a>
  <a href="benchmarks/RESULTS.md"><img alt="Benchmarks, with their conditions" src="https://img.shields.io/badge/benchmarks-with%20their%20conditions-C6F16B?labelColor=0D0F13"></a>
  <a href="LICENSE"><img alt="Code license: Apache-2.0" src="https://img.shields.io/badge/code-Apache--2.0-C6F16B?labelColor=0D0F13"></a>
  <a href="MODEL_LICENSE.md"><img alt="Model license" src="https://img.shields.io/badge/weights-free%20for%20commercial%20use-5EEAD4?labelColor=0D0F13"></a>
</p>

<p align="center">
  <a href="https://purebyte.ai"><img alt="Website" src="https://img.shields.io/badge/web-purebyte.ai-C6F16B?labelColor=0D0F13"></a>
  <a href="https://x.com/purebyteai"><img alt="X" src="https://img.shields.io/badge/X-@purebyteai-0D0F13?logo=x&logoColor=white"></a>
  <a href="https://www.youtube.com/@purebyteai"><img alt="YouTube" src="https://img.shields.io/badge/YouTube-@purebyteai-0D0F13?logo=youtube&logoColor=FF0000"></a>
  <a href="https://www.instagram.com/purebyte.ai/"><img alt="Instagram" src="https://img.shields.io/badge/Instagram-@purebyte.ai-0D0F13?logo=instagram&logoColor=E4405F"></a>
  <a href="https://www.tiktok.com/@purebyte"><img alt="TikTok" src="https://img.shields.io/badge/TikTok-@purebyte-0D0F13?logo=tiktok&logoColor=white"></a>
  <a href="https://www.reddit.com/user/purebyteai/"><img alt="Reddit" src="https://img.shields.io/badge/Reddit-u%2Fpurebyteai-0D0F13?logo=reddit&logoColor=FF4500"></a>
  <a href="https://sirventai.medium.com"><img alt="Blog" src="https://img.shields.io/badge/blog-sirventai.medium.com-0D0F13?logo=medium&logoColor=white"></a>
  <a href="mailto:pablo@purebyte.ai"><img alt="E-mail" src="https://img.shields.io/badge/e--mail-pablo@purebyte.ai-5EEAD4?labelColor=0D0F13"></a>
</p>

<p align="center">
  <a href="#what-purebyte-is">What it is</a> ·
  <a href="#get-started">Get started</a> ·
  <a href="#what-you-can-build">What you can build</a> ·
  <a href="#how-it-works">How it works</a> ·
  <a href="#purebyte-laya-and-typesafe-jev">vs Laya and Jev</a> ·
  <a href="#three-example-specialists">Examples</a> ·
  <a href="#train-your-own-vibe-training">Train your own</a> ·
  <a href="#compatible-machines">Machines</a> ·
  <a href="#configure">Configure</a> ·
  <a href="#the-future-of-purebyte-a-marketplace-of-ai-specialists">Future</a> ·
  <a href="#faq">FAQ</a> ·
  <a href="#author">Author</a> ·
  <a href="https://github.com/purebyte-ai/purebyte-train">Training repository</a>
</p>

<table align="center">
  <tr>
    <td align="center" width="33%"><h2>0.82 ms</h2>per 64-byte decision<br>on a 12-core desktop CPU</td>
    <td align="center" width="33%"><h2>133×</h2>less time than Laya for one decision<br>on the same CPU (1.9 M vs 421 M parameters)</td>
    <td align="center" width="33%"><h2>1–29 MiB</h2>per model: ternary weights<br>and 4-bit lookup tables</td>
  </tr>
  <tr>
    <td align="center"><h2>25 min</h2>to train three seeds and a control<br>from scratch on one consumer GPU</td>
    <td align="center"><h2>Bit-exact</h2>on every CPU kernel and thread count<br>of a platform</td>
    <td align="center"><h2>3 examples</h2>ahead of OpenAI's Privacy Filter (PIIMB),<br>gitleaks (CredData) and a strings + regex baseline</td>
  </tr>
</table>

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/hero-dark.png">
    <img src="docs/assets/hero-light.png" alt="Four panels. Time for one decision on a short input: PureByte on a desktop CPU 2.8 ms, Laya on a T4 GPU 33 to 40 ms, TypeSafe Jev hosted API 236 to 276 ms, Laya on the same CPU 373 ms. Model size in parameters: the PureByte examples 1.9 M, 8.3 M and 54 M, Laya 421 M, openai/privacy-filter 1.4 B (about 50 M active). Training on one consumer GPU, every seed of the recipe: secrets-code 24.5 minutes, secrets-bin 58 minutes, pii 60 minutes, against 4 to 5 hours of fine-tuning for Laya on two T4 GPUs (different GPUs and different jobs: Laya fine-tunes a general model). Three example specialists, each against a baseline: pii 0.769 against openai/privacy-filter 0.662 on the PII Masking Benchmark, secrets-code 0.797 against gitleaks 0.337 on CredData, secrets-bin recall 1.000 against 0.803 for strings plus regular expressions on a synthetic probe made by its training generator." width="100%">
  </picture>
</p>

---

## What PureByte is

PureByte is an architecture, a runtime and a training stack for **AI Specialists**: small models, each trained for one
job, that read raw bytes (text, source code, logs, JSON, binaries, archives, network payloads) and answer with
**decisions, scores, labels and byte ranges instead of generated text**. Nothing is generated, so nothing can be
invented (a model can still miss something or flag something harmless). The models run on ordinary CPUs, offline, with
the same bits from every kernel and thread count of a platform, and the same decisions and spans expected across
platforms, and you can
train one for your own task on a consumer GPU, or describe it to an AI coding agent and let it do the training:
[**Vibe Training**](#train-your-own-vibe-training).

| Part | What it is | Where |
|---|---|---|
| **The architecture** | A byte embedding, an optional hashed n-gram memory, a stack of ternary Mamba-2-style state-space blocks, and heads that decide, score, label bytes or map them; constrained decoding; no tokenizer, no generation | [How it works](#how-it-works), [docs/architecture.md](docs/architecture.md) |
| **The runtime** | One dependency-free C++17 engine that runs any model declared by a single GGUF file, bit-exact on scalar, AVX2 and NEON kernels; a CLI, a local HTTP API, Python bindings and a C API | [engine/](engine/), [cli/](cli/), [spec/FORMAT.md](spec/FORMAT.md) |
| **The training stack** | `pbtrain`: data generators grounded in real bytes, leak checks, criteria written before training, multi-seed runs, export and proof that the runtime gives the same answers; trains on a CPU or a GPU (tested on an AMD Radeon; NVIDIA, Linux ROCm and Apple silicon run the same stock PyTorch code, not yet run by the maintainers) | [purebyte-ai/purebyte-train](https://github.com/purebyte-ai/purebyte-train), [the guide](https://github.com/purebyte-ai/purebyte-train/blob/main/docs/training.md) |
| **Three example specialists** | `pii`, `secrets-code` and `secrets-bin`: what one architecture does on three different jobs | [Examples](#three-example-specialists), [models/](models/README.md) |

To show what the architecture can do, this release ships three example specialists: two for jobs with public
benchmarks and strong incumbents, and one for a job that has no public benchmark yet. On the PII Masking Benchmark
`pii` scores above OpenAI's Privacy Filter (mean F2 0.769 against 0.662) with 170 times fewer parameters in total (6
times fewer than its about 50 M active); on CredData `secrets-code` finds three and a half times as many labeled credential
lines as gitleaks at a close precision (2.9 times on the original values, which the benchmark obfuscates); and `secrets-bin` finds credentials inside compiled binaries, measured on our
own exams. They are examples of what PureByte can do, not the limit of it: the runtime runs any model in the format,
and the training stack builds span-finding specialists today, with decisions, labels, scores and maps next
([roadmap](docs/roadmap.md)).

## Why PureByte

| | What you get | The proof |
|---|---|---|
| **Runs on almost any CPU** | No GPU, no CUDA, no Python at run time: self-contained release binaries for x86-64 (Linux, macOS, Windows) and arm64 (Linux, macOS) with AVX2 and NEON kernels, and a portable C++17 build for any other little-endian CPU | [Install](docs/install.md) |
| **Millisecond decisions** | 0.82 ms (`secrets-code`) to 1.45 ms (`secrets-bin`) for a 64-byte input with 8 threads of a 12-core desktop CPU; about 2,700 small decisions per second in batches of 48 | [Speed](#speed-and-footprint) |
| **Megabytes, not gigabytes** | Ternary weights (−1, 0 or +1, stored in 2.25 bits with their scales) and 4-bit lookup tables; 1 to 29 MiB per model; 9 to 40 MB of memory to answer a small input | [Model cards](models/README.md) |
| **Cheap to train** | Trained from scratch on one consumer GPU, no pretrained body: 25 minutes for three seeds of a small model, about 2.5 GPU-hours for all three examples; on a CPU, the `pii` smoke recipe trains and exports a tiny model on made-up data in about two minutes | [Vibe Training](#train-your-own-vibe-training) |
| **Reads anything** | Raw bytes: prose, code, minified JavaScript, base64, UTF-16, logs, machine code, archives; answers with exact byte offsets | [Why bytes](#why-bytes) |
| **Linear in the input** | State-space blocks read in one pass with a fixed-size state: four times the bytes cost four times the work (attention's cost grows with the square); the format also has a stream mode for inputs of any length, which no released example uses yet | [Long inputs](#long-inputs-and-longer-windows) |
| **Cannot invent content** | It can miss things or flag harmless ones, but every answer is a declared label, a score or a byte range of your input; redactions are copies of your input, checked byte for byte | [How it works](#how-it-works) |
| **Exact and reproducible** | Same bytes, same answer, bit for bit, on every kernel (scalar, AVX2, NEON) and at any thread count of a platform; across platforms, the same decisions and spans are expected, and golden tests check them | [Architecture](docs/architecture.md#numerics-and-exactness) |
| **Private by construction** | Models run inside your process; nothing leaves the machine; no telemetry | [Privacy](docs/privacy.md) |
| **One runtime, any model** | A model is one declarative GGUF file; blocks, heads and post-processing profiles come from registries, so a new specialist needs no new runtime | [spec/FORMAT.md](spec/FORMAT.md) |
| **Measured honestly** | Public benchmarks where they exist, criteria written before training and every verdict published, failures and changes included; three to six seeds per recipe; leak checks down to 64-byte blocks; competitors in the same harness where we can run them | [Methodology](#how-we-measure) |
| **Fits your stack** | CLI, local HTTP API, Python, a C API with a stable ABI, SARIF, pre-commit, GitHub Action, GitLab CI, Docker, VS Code | [Use it](#use-it) |
| **Open** | Architecture, runtime, format, training stack, recipes and data scripts under Apache-2.0; the example weights are free for commercial use | [License](#license) |

## Get started

```bash
pip install purebyte                      # Linux, macOS, Windows · Python 3.8+
purebyte models pull pii                  # an example specialist; the download is checked against its SHA-256
purebyte models pull secrets-code
purebyte models pull secrets-bin
```

`pip` brings the native runtime and the `purebyte` command; `purebyte models pull` downloads with `curl`, which macOS,
Windows 10 (1803 and later) and most Linux distributions include, but slim container images do not. A release binary
needs no Python at all. Every [release](https://github.com/purebyte-ai/purebyte/releases) has a
self-contained binary for Linux, macOS and Windows, there is a [Docker image](integrations/docker/README.md)
(`ghcr.io/purebyte-ai/purebyte`), and the source builds with CMake and any C++17 compiler (on Windows, MSVC 19.14, Visual Studio 2017 version 15.7, or
newer). Offline machines, checksums
and every option: [docs/install.md](docs/install.md).

Then try the examples on your own files:

```bash
purebyte redact --model pii ticket.txt                  # personal data masked, the rest copied byte for byte
purebyte scan src/                                      # leaked credentials in code (secrets-code is the default)
purebyte scan --model secrets-bin dist/                 # credentials inside executables, libraries, APKs, JARs
echo 'db_password = "Tr0ub4dor&3xyz"' | purebyte decide --model secrets-code -    # one small decision: yes or no
purebyte bench --model pii                              # latency and throughput on your machine
```

Real runs of each one, with their outputs, are in [Three example specialists](#three-example-specialists).

## What you can build

A model is defined by what it reads (any bytes) and what it answers. The format and the runtime support all of these
answers today; the training stack builds the span answers of the first two rows today, and the others are next on its
[roadmap](docs/roadmap.md):

| The model answers | Jobs it fits | In this release |
|---|---|---|
| **Byte ranges** (typed spans) | Where are the credentials, names, IBANs, IP addresses, stack traces, SQL fragments, license keys | The three examples: [`pii`](#pii-personal-data-masked-anywhere), [`secrets-code`](#secrets-code-leaked-credentials-in-code), [`secrets-bin`](#secrets-bin-credentials-inside-binaries) |
| **A redacted copy** | The same file with every span replaced by a marker, verified to be your input everywhere else | The `redact` profile, used by `pii` |
| **A decision** (one of N) | Is this prompt an injection? Is this log line an alert? Is this request an attack? Which format or language is this file? | Runtime: `choice` head and `purebyte decide`; training: next |
| **Several labels at once** | Which kinds of sensitive data does this document hold? | Runtime: `multilabel` head; training: next |
| **A score or a grade** | The risk of a transaction description, the severity of an event | Runtime: `score` and `ordinal` heads; training: next |
| **A value, by asking for it** | Where is the password in this configuration? Where is the total of this invoice? | Runtime: query templates and span heads; training: next |
| **A per-byte map** | How suspicious each byte of a file looks; where the code and data regions of a binary are | Runtime: `byte_map` head; training: next |

And it runs wherever the bytes are: in a pre-commit hook, a CI job, a build pipeline, a log shipper, a gateway in
front of a hosted LLM, a data pipeline that cleans datasets, a desktop application through the C API, or an arm64
server.

| Use of the examples today | How | Status |
|---|---|---|
| Stop credentials before they are committed | The [pre-commit hook](integrations/pre-commit/): staged changes in about a second | Ready |
| Code scanning alerts in every pull request | The [GitHub Action](integrations/github-action/) or [GitLab CI](integrations/gitlab-ci/) job, SARIF inline in the diff | Ready |
| Check what you ship: builds, containers, APKs, JARs, firmware | `purebyte scan --model secrets-bin dist/`, archives opened | Ready |
| Redact logs, tickets and exports before they leave the host | `purebyte redact --model pii`, or `POST /v1/redact` on the [local HTTP API](docs/api.md) | Ready |
| Mask personal data before a prompt reaches a hosted LLM | The local HTTP API in front of your LLM client; consistent markers keep the prompt readable | Ready |
| Clean datasets before training or sharing them | One redaction per file, consistent markers inside each file and a reversible map (markers restart in every file) | Ready |

### Long inputs and longer windows

- **Linear cost.** State-space blocks read a window in one pass with a fixed-size state, so the work grows linearly
  with the input: on one thread a window of 1,024 bytes costs 4.2 times one of 256 bytes, where attention would pay
  up to 16 times.
- **Stream any length.** Besides windows, the format declares a stream context: a causal model trained for it reads
  an input of any length in consecutive chunks and carries each block's state from chunk to chunk, and the hidden
  states are exactly those of one pass over the whole input ([spec/FORMAT.md](spec/FORMAT.md) §12.4). No released example
  uses it yet.
- **Train longer windows.** Training scales the same way: a `nano` model with 1,024-byte windows and batches of 32
  trains in 5.9 GB of GPU memory ([training/docs/gpu.md](https://github.com/purebyte-ai/purebyte-train/blob/main/training/docs/gpu.md)). The three examples use 512-byte
  windows every 384 bytes, which fit a line of code with its context and keep each decision small.
- **Reading far is not remembering far.** A fixed-size state summarizes what it has read; it does not store it. A
  model can read a megabyte in one stream, but recalling an exact detail from far back needs an exact memory next to
  the state, such as an attention block over a local window plus a few selected positions. That is on the
  [roadmap](docs/roadmap.md), and we will claim long-context recall only after measuring it on a real long-context
  benchmark.

## How it works

```mermaid
flowchart LR
    subgraph READ["Read"]
        direction TB
        A["Bytes<br/>a file, a diff, a request"] --> B["Windows, or one stream<br/>with carried state"]
    end
    subgraph MODEL["Model"]
        direction TB
        N["N-gram memory<br/>hashed tables, 4-bit, optional"] --> C["Byte embedding<br/>256 x d"]
        C --> D["Ternary Mamba-2 blocks<br/>weights in -1, 0, +1"]
        D --> E["Heads<br/>decide · score · label bytes · map"]
    end
    subgraph ANSWER["Answer"]
        direction TB
        F["Decisions, scores, spans<br/>constrained decoding"] --> G["Profile<br/>merge, rules, vote, mask, redact"]
        G --> H["JSON · JSONL · SARIF<br/>redacted copy"]
    end
    READ --> MODEL --> ANSWER
```

1. **Bytes in, no tokenizer.** Each byte selects a row of a 256-row embedding. An optional **n-gram memory** adds, for
   every position, rows of large hashed tables looked up by the last 2, 3 and 4 bytes: the cheapest way to give a
   small model a lot of knowledge about short patterns. The tables are stored at 4 bits per value and cost three row
   reads per byte, whatever their size.
2. **Ternary state-space blocks.** A stack of Mamba-2-style selective state-space blocks reads the input in one
   linear pass with a fixed-size state. Their large projections are ternary: every weight is −1, 0 or +1 with one
   scale per group of weights (128 in the released examples), stored in 2.25 bits, trained that way from the first
   step.
3. **Heads, not generation.** Small float heads turn the hidden states into answers: a decision over the whole window
   (which can also gate the other heads), labels for several properties, scores, grades, per-byte labels decoded into
   consistent spans by a constrained Viterbi pass, or per-byte maps. An operating point stored in the model moves it
   toward precision or recall without retraining.
4. **Windows, streams and threads.** Inputs are read in overlapping windows (independent, so they run in parallel) or
   as one stream with carried state. When there are fewer windows than threads, the threads share the work of one
   window, which is what makes one small decision fast.
5. **Profiles.** The model file declares its post-processing: merging windows, format rules, an ensemble vote,
   masking, or redaction by copy.
6. **Exact by contract.** The numerics are specified down to the order of operations: the scalar kernel is the
   executable specification, and the AVX2 and NEON kernels reproduce it bit for bit, at any thread count.

### Design choices, and the evidence behind them

| Choice | Why | What we measured |
|---|---|---|
| Bytes, no tokenizer | Exact offsets; every format, encoding and binary is just bytes | The same architecture learned credentials in code, credentials in machine code and personal data in 23 languages |
| State-space blocks | Linear cost and a fixed-size state; streaming | On one thread, a window of 1,024 bytes costs 4.2 times one of 256; a bidirectional variant tied with the causal model on the PII Masking Benchmark (three seeds each), and it cannot stream |
| Ternary weights | 2.25 bits per weight; trained from the first step | On 24 clean binaries, a float control made 2.4 times fewer false alarms than the same ternary model, and adding the n-gram memory cut the ternary model's 13 times; a float model with the memory was not trained, so ternary plus memory is what we ship, not a proven optimum |
| Hashed n-gram memory | Knowledge in lookup tables, three reads per byte | False alarms on 24 clean binaries fell from 134 to 10 (means of three seeds) with 50 M table entries; storing the tables at 4 bits shrank the file from 194 to 29 MiB and changed the spans of about 1 % of test windows |
| A window decision that gates the spans | "Is there anything here?" decided once, with max pooling | On a synthetic task where the evidence comes after the span, span-F1 rose from 0.67 to 0.99 for 0.003 % more compute (two seeds); where the evidence is inside the span, from 0.984 to 0.994 |
| Answers, not generation | Nothing to parse, nothing to invent | Every redaction is checked to equal the input outside the spans before it is returned |
| An exact runtime | The same bits on one platform, whatever the kernel or thread count; tests can compare exactly | Golden and invariance tests on every change; each released file gave the same spans in the engine as in PyTorch before release (2,000 of 2,000 test windows for the credential examples, 40 of 40 for `pii`) |
| Threads sharing one window | Latency of a single small decision | 2.97 ms on one thread, 0.82 ms on eight, for one 64-byte window |

The ternary, n-gram, gating and bidirectional rows are research runs of September 2026, not release measurements:
their models, data, seeds, dates and what the public trainer can repeat are in
[benchmarks/RESULTS.md](benchmarks/RESULTS.md#design-experiments); the speed rows are in
[docs/performance.md](docs/performance.md).

### One runtime, any specialist

```mermaid
flowchart LR
    subgraph Make["Made with purebyte-train"]
        T1["Recipe and data<br/>specialists/ · data/"] --> T2["Training stack<br/>pbtrain"]
    end
    subgraph Files["AI Specialists are files"]
        S1["pii.gguf"]
        S2["secrets-code.gguf"]
        S3["secrets-bin.gguf"]
        S4["your-model.gguf"]
    end
    subgraph Runtime["purebyte runtime"]
        R1["Registries<br/>blocks · heads · profiles"]
    end
    T2 --> S4
    S1 --> R1
    S2 --> R1
    S3 --> R1
    S4 --> R1
    R1 --> O1["Decisions and labels"]
    R1 --> O2["Scores"]
    R1 --> O3["Typed spans"]
    R1 --> O4["Per-byte maps"]
    R1 --> O5["Redacted copies"]
```

The runtime knows nothing about any task. Each model file declares its window or stream, its blocks (`ssm_v2`,
`attention`), its heads (`choice`, `multilabel`, `score`, `ordinal`, `tag`, `byte_map`, `exit`), its output labels and
its post-processing profile, and the runtime assembles it from registries. A file that asks for something the runtime
does not implement is refused with a message that names it, never guessed. Adding a block, a head or a profile is one
module and one line in a registry.

### Why bytes?

A tokenizer is a vocabulary of word pieces learned from prose. Feed it an API key, a base64 blob or a PE header and it
shatters the input into arbitrary fragments, and every position has to be mapped back approximately. A byte model sees
exactly what is there, answers with exact offsets, reads UTF-16, minified code and machine code as easily as prose,
and needs only 256 input symbols, which is one reason the models are so small.

The plain-language tour: [docs/how-it-works.md](docs/how-it-works.md). The engineering:
[docs/architecture.md](docs/architecture.md). The normative definition of every block, head and number:
[spec/FORMAT.md](spec/FORMAT.md).

## Speed and footprint

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/speed-threads-dark.png">
    <img src="docs/assets/speed-threads-light.png" alt="Median latency of one window by thread count, for windows of 64, 256 and 1,024 bytes. With 8 threads: secrets-code 0.82, 2.8 and 10.5 ms; pii 0.89, 3.1 and 13.7 ms; secrets-bin 1.45, 5.0 and 24.5 ms." width="100%">
  </picture>
</p>

The three example models on an idle 12-core desktop CPU (AMD Ryzen 9 5900X), with the release build:

| | `secrets-code` (4 blocks) | `pii` (4 blocks + memory) | `secrets-bin` (8 blocks + memory) |
|---|---:|---:|---:|
| One 64-byte window (8 threads) | **0.82 ms** | **0.89 ms** | 1.45 ms |
| One 1,024-byte window (8 threads; twice the examples' window) | 10.5 ms | 13.7 ms | 24.5 ms |
| Small decisions per second (batches of 48 inputs of 64 bytes, 12 threads) | 2,700 | 2,600 | 1,300 |
| Throughput on whole files | 131 KB/s | 126 KB/s | 65 KB/s |
| Start, load and answer once (Linux) | 0.01 s | 0.03 s | 0.16 s |
| Peak memory: one small decision / a large scan (Linux, 23 threads) | 9.4 MB / 188 MB | 13 MB / 182 MB | 39 MB / 220 MB |

A small decision takes about a millisecond (0.82 to 1.45 ms) and every thread count gives bit-identical results. Large inputs cost
time linearly: a neural network reads every byte, so for whole repositories scan once, then scan changes, and keep a
pattern scanner, which is much faster per byte, as a first layer. Tables, conditions and how to measure on your
machine: [docs/performance.md](docs/performance.md).

## PureByte, Laya and TypeSafe Jev

[Laya](https://huggingface.co/convaiinnovations/laya) and TypeSafe Jev are non-generative decision models: you describe a question and its options, and they answer in a single pass, without writing text. PureByte
starts from the same idea, decisions instead of generation, and takes it all the way down: to bytes, to megabytes and
to the CPU.

| | **PureByte** | Laya | TypeSafe Jev |
|---|---|---|---|
| What it is | An architecture for tiny specialists, one job each, and the runtime that runs them | A general decision model: typed questions about a text | A hosted decision model |
| Runs on | **x86-64 and arm64 CPUs**, one self-contained binary | PyTorch; its published speed is on a GPU | The vendor's servers |
| One decision on a short input | **0.82 ms (64 bytes) to 2.8 ms (256 bytes)**, 8 threads of a desktop CPU | 33 to 40 ms on a T4 GPU; **373 ms on the same CPU** | 236 to 276 ms (p50, third-party) |
| Parameters | **1.9 M to 54 M** (the three examples) | 421 M (English checkpoint) | Not published |
| Download | **1 to 29 MiB** per model | 808 MB (English checkpoint) | None: an API |
| Input | **Any bytes**: text, code, binaries, UTF-16, logs | Text, through a tokenizer | Text |
| Answer | Decisions, scores, labels, exact byte ranges, redacted copies | A choice, a score or a probability per question | A choice or a score |
| Training | **From scratch on one consumer GPU**: 25 minutes for three seeds of `secrets-code` | Fine-tuning notebook for two T4 GPUs (about 4 to 5 hours for 4 epochs over about 30,000 questions, by [Laya's README](https://github.com/NandhaKishorM/laya)), on top of ModernBERT-large, a 395 M encoder pretrained on 2 trillion tokens ([its model card](https://huggingface.co/answerdotai/ModernBERT-large)) | Not available |
| Your data | **Never leaves the machine** | Stays with you if self-hosted | Sent to the API |
| Same input, same output | **Bit for bit** on a platform, on every kernel and thread count | Not stated | Not stated |
| Cost per decision | Nothing | Nothing if self-hosted, on your GPU | A price per request (hosted API) |

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/speed-dark.png">
    <img src="docs/assets/speed-light.png" alt="Median latency of one small decision, log scale. PureByte secrets-code on a desktop CPU: 0.82 ms for 64 bytes with 8 threads, 2.8 ms for 256 bytes, 10.5 ms for 1,024 bytes, 12 ms for 256 bytes on one thread. Laya on a T4 GPU, its own figure: 33 to 40 ms. TypeSafe Jev hosted API, third-party p50: 236 to 276 ms. Laya on the same CPU: 373 ms with 12 threads, 1.4 s on one thread." width="100%">
  </picture>
</p>

**How we compared.** PureByte and Laya ran on the same idle AMD Ryzen 9 5900X: Laya is the `laya` 0.3.6 package with
its English checkpoint on PyTorch 2.14, answering one three-option question about one line of code (173 bytes, 190
tokens with the question); PureByte is the `secrets-code` example on one window of 256 bytes, which holds that line with
room to spare. Laya's GPU figure and Jev's figures are the ones Laya publishes. The conditions and every thread count
are in [docs/performance.md](docs/performance.md#compared-with-laya-and-jev). The comparison is of time, not of
quality, and the jobs differ: a 1.9 M-parameter specialist for one fixed question against a 421 M-parameter general
model that reads its question at request time.

**Where Laya and Jev lead.** They answer questions written at request time: a new decision needs no training, while
a PureByte model answers the question it was trained for, and a new one means training one (25 minutes on a consumer
GPU, see [Vibe Training](#train-your-own-vibe-training)). Laya's multilingual checkpoint reads more than 100
languages, where the `pii` example learned 23; Laya reports probabilities calibrated by temperature fitting (mean ECE
0.081 on its benchmark, by its [model card](https://huggingface.co/convaiinnovations/laya)), where PureByte's
confidence is not calibrated yet; and both are measured on general text classification benchmarks, where PureByte has
not been.

## Three example specialists

To test the architecture on real work, we trained it for three jobs: two with public benchmarks and strong incumbents
(PIIMB, CredData), and one, credentials inside binaries, that has no public benchmark yet. We release the results,
failed criteria included. They are examples: each is one model file, trained with the stack in
[purebyte-train](https://github.com/purebyte-ai/purebyte-train), and none needed a change to the runtime.

| Example | What it does | Size | Result |
|---|---|---|---|
| **`pii`** | Finds and masks personal data in prose, code, JSON, CSV, SQL and logs, in 23 languages | 4.5 MiB · 8.3 M parameters | **PII Masking Benchmark: F2 0.769**, above OpenAI's Privacy Filter (0.662, 1.4 B parameters, about 50 M active) |
| **`secrets-code`** | Finds leaked credentials in source code and configuration, passwords included | 0.98 MiB · 1.9 M parameters | **CredData: F1 0.797**, against 0.337 for gitleaks and 0.047 for trufflehog; on the original values, 0.694 against 0.329 |
| **`secrets-bin`** | Finds credentials inside executables, libraries, bytecode, APKs, JARs, WebAssembly and firmware | 28.8 MiB · 54 M parameters | **360 of 360** credentials found on a synthetic probe made by its own training generator (95 % interval 0.990–1.000); 0 to 12 false alarms per set of clean binaries |

### `pii`: personal data, masked, anywhere

```bash
cat > ticket.txt <<'EOF'
From: Laura Gómez <laura.gomez@example.com>
Phone: +34 612 345 678

Hi, please send the invoice for order 4471 to Calle Mayor 12, 28013 Madrid.
You can charge my account ES91 2100 0418 4502 0005 1332.

2026-09-24 10:31:07 INFO login ok user=laura.gomez ip=192.168.1.24
{"customer": "Laura Gómez", "email": "laura.gomez@example.com", "dni": "12345678Z", "plan": "pro"}
EOF

purebyte redact --model pii ticket.txt
```

```text
From: [PII_1] <[PII_2]>
Phone: [PII_3]

Hi, please send the invoice for order [PII_4] to [PII_5].
You can charge my account [PII_6].

[PII_7] INFO login ok user=[PII_8] ip=[PII_9]
{"customer": "[PII_1]", "email": "[PII_2]", "dni": "[PII_10]", "plan": "pro"}
purebyte: 12 span(s) redacted; outside them the output is identical to the input (checked)
```

A real run of the released model on made-up data. Prose, logs and JSON are handled in one pass; the same value always
gets the same marker (`Laura Gómez` is `[PII_1]` in the text and inside the JSON), so the copy still reads and joins
still work; and the output is your input byte for byte outside the masked spans, checked before it is written.
`--map` writes a reversible map (keep it secret) and `purebyte redact --restore` puts the values back. It masks one
type, `PII`, with the policy of the [PII Masking Benchmark](https://huggingface.co/datasets/piimb/pii-masking-benchmark)
(PIIMB): dates and identifiers such as the order number count as personal data; `--bias` trades recall for precision.

On PIIMB, a public benchmark with a leaderboard (150,022 sentences in six tasks, character-level F2: β = 2, recall
weighted above precision), the model learned from 387,391 labeled synthetic documents from public datasets, in 23
languages, and from generated records, logs and code:

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/bench-piimb-dark.png">
    <img src="docs/assets/bench-piimb-light.png" alt="PII Masking Benchmark, mean F2 against model parameters on a log scale, 36 models. PureByte pii, 8.3 M parameters and 4.5 MiB, scores 0.769: smaller than every model on the leaderboard and ahead of 23 of its 35. Every model that scores higher has at least 68 M parameters. openai/privacy-filter, 1.4 B parameters, scores 0.662; Presidio with spaCy 0.586; the leaderboard's first, 1.4 B parameters, 0.836." width="100%">
  </picture>
</p>

| | Parameters | Mean F2 of 6 tasks | ai4privacy-en | Mean precision |
|---|---|---:|---:|---:|
| **PureByte `pii` 1.0.0** (the released file) | **8.3 M** (4.5 MiB) | **0.769** | **0.957** | **0.773** |
| PureByte `pii`, six training seeds: mean [range] | 8.3 M | 0.777 [0.769–0.785] | 0.962 | 0.765 |
| openai/privacy-filter | 1.4 B (about 50 M active) | 0.662 | 0.843 | 0.751 |
| Presidio + spaCy `en_core_web_lg` | 106 M | 0.586 | | |
| The leaderboard's first (OpenMed privacy-filter-multilingual-v2) | 1.4 B (about 50 M active) | 0.836 | 0.975 | 0.832 |

Above OpenAI's Privacy Filter on all three numbers, with 170 times fewer parameters in total (6 times fewer than its
about 50 M active, per its model card), on a CPU. Not first: twelve
models score higher, from 68 M to 1.4 B parameters, and most of the gap is in two tasks, protocol traces (privy) and
EU legal texts (mapa-eur-lex), which resemble nothing the model learned from:

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/bench-piimb-tasks-dark.png">
    <img src="docs/assets/bench-piimb-tasks-light.png" alt="PII Masking Benchmark F2 by task, PureByte pii against openai/privacy-filter and the leaderboard's first: ai4privacy-en 0.96, 0.84, 0.98; ai4privacy-multi 0.95, 0.81, 0.97; nemotron-pii 0.89, 0.61, 0.91; gretel 0.95, 0.87, 0.97; privy 0.54, 0.54, 0.82; mapa-eur-lex 0.33, 0.31, 0.37." width="100%">
  </picture>
</p>

Because the architecture reads bytes, the same model can learn structured inputs as easily as prose. The released
recipe mixes generated records, logs and code into the public documents, and on a test set of held-out formats and
values it leaves far fewer values unmasked than the same model trained on the documents alone:

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/pii-contexts-dark.png">
    <img src="docs/assets/pii-contexts-light.png" alt="Personal data left unmasked by kind of input, trained on public documents only against the released recipe: code 38 % to 1.3 %, structured records 28 % to 1.9 %, logs 21 % to 0.04 %, e-mails 21 % to 19 %, prose 27 % to 27 %." width="100%">
  </picture>
</p>

That test set comes from the same generator family as the generated training data, so these gains are optimistic;
the PIIMB figures above come from other documents (four of its six tasks, though, come from the same public synthetic
datasets as most of the training pool). We ship this recipe even though it scores 0.007 lower on PIIMB than the
documents-only one (0.779 against 0.786 over three seeds): a tie by the pre-registered rule, which keeps the mix
measured first, so shipping this one was a product decision, and choosing the lower-scoring model on the benchmark
inflates nothing. Every seed, the second operating point and the overlap analysis:
[benchmarks/RESULTS.md](benchmarks/RESULTS.md#pii-on-the-pii-masking-benchmark).

### `secrets-code`: leaked credentials in code

```bash
cat > settings.py <<'EOF'
import os

DATABASE_URL = "postgres://app:KcZjR4I0b3jRta@db.internal:5432/app"
DB_PASSWORD = "Tr0ub4dor&3xyz"
SMTP_PASSWORD = os.environ["SMTP_PASSWORD"]
SIGNING_KEY = "8f2Kq9LmX4vR7tZp1nB6cW3yH5jD0sGa"
AWS_ACCESS_KEY_ID = "AKIAIOSFODNN7EXAMPLE"  # the example key from the AWS docs
EOF

purebyte scan settings.py | jq -c '.findings[] | {line, col, kind, snippet_masked}'
```

```text
purebyte: 1 file(s), 0.3 KiB, 3 finding(s) (3 error(s), 0 warning(s)), 0.0 s, 0 not scanned
{"line":3,"col":17,"kind":"secret","snippet_masked":"post************pp"}
{"line":4,"col":16,"kind":"secret","snippet_masked":"Tr************"}
{"line":6,"col":16,"kind":"secret","snippet_masked":"8f2K************Ga"}
```

Three made-up credentials, masked, at exact positions. The password on line 4 has no fixed format that a rule could
match: the model finds it from its context. What it does **not** flag matters as much: line 5 is named like a
password but only reads the environment, and line 7, the example key from AWS's documentation, is dropped by a rule of
the `secrets-code` profile (`--keep-examples` reports it). `purebyte scan`
exits with status 1 when it finds something, so a hook or a CI job stops; findings in test, example and
documentation paths are warnings that do not stop it unless you add `--strict`.

On [Samsung CredData](https://github.com/Samsung/CredData), a public benchmark of labeled credentials (164
repositories of its TEST half, 4,219 labeled credential lines), with every tool run by us on the same files:

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/bench-creddata-dark.png">
    <img src="docs/assets/bench-creddata-light.png" alt="F1 on CredData TEST with 95 % bootstrap intervals: PureByte secrets-code 0.797, ensemble of three 0.805, gitleaks 0.337, trufflehog without verification 0.047." width="100%">
  </picture>
</p>

| Tool | Found | False positives | Missed | Precision | Recall | F1 [95 % interval] |
|---|---:|---:|---:|---:|---:|---|
| **PureByte `secrets-code` 1.0.0** | **3,077** | 422 | 1,142 | 0.879 | **0.729** | **0.797** [0.719–0.860] |
| PureByte, ensemble of three | 3,128 | 429 | 1,091 | 0.879 | 0.741 | 0.805 [0.731–0.863] |
| gitleaks 8.30.1 | 875 | 92 | 3,344 | 0.905 | 0.207 | 0.337 [0.263–0.421] |
| trufflehog 3.97.6 (no verification) | 104 | 76 | 4,115 | 0.578 | 0.025 | 0.047 [0.017–0.092] |

At a precision close to gitleaks' (0.88 against 0.91), a 1.9 M-parameter model finds three and a half times as many
labeled credentials. CredData ships its credentials with random values in place of the real ones, and the model
learned from such values: with the original values restored, it finds 2.9 times as many as gitleaks (2,460 against
850; F1 0.694 against 0.329), and misses more of the passwords that are ordinary words
([benchmarks/RESULTS.md](benchmarks/RESULTS.md#on-the-original-values)). The difference is in what has no fixed format,
which is where a model that reads context helps:

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/bench-creddata-families-dark.png">
    <img src="docs/assets/bench-creddata-families-light.png" alt="Recall by family of credential, PureByte secrets-code, gitleaks and trufflehog: passwords 0.88, 0.06, 0.00; tokens 0.69, 0.44, 0.00; URL credentials and Basic auth 0.99, 0.01, 0.08; provider formats 0.85, 0.28, 0.05; generic keys 0.82, 0.49, 0.00; private keys 0.90, 0.84, 0.14; UUIDs, salts and nonces 0.12, 0.00, 0.00." width="100%">
  </picture>
</p>

The confidence intervals, the slices, the false alarms on ordinary repositories and every caveat (the training-data
overlaps that we found, measured and left out of the published figures, and the recipe's pre-registered criteria, two
of which failed) are in [benchmarks/RESULTS.md](benchmarks/RESULTS.md#secrets-code-on-creddata).

### `secrets-bin`: credentials inside binaries

```bash
purebyte scan --model secrets-bin report-service.exe | jq -c '.findings[] | {offset_hex, string_masked}'
```

```text
purebyte: 1 file(s), 20.0 KiB, 3 finding(s) (3 error(s), 0 warning(s)), 0.4 s, 0 not scanned
{"offset_hex":"0x2a9e","string_masked":"client_secret=Zq7v************gA"}
{"offset_hex":"0x2b16","string_masked":"Authorization: Bearer eyJh************Ng"}
{"offset_hex":"0x2bc0","string_masked":"post************al:5432/reports"}
```

A small C program compiled with three made-up credentials in its strings. Because the architecture reads bytes, the
same design that reads code reads machine code: executables (PE, ELF, Mach-O), libraries, object files, Java and
Python bytecode, WebAssembly and firmware, with no disassembler and no tokenizer. Archives are opened: point it at a
`.jar`, an `.apk`, a wheel or a `.tar.gz` and members are reported as `outer.jar!/inner/path`.

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/bench-secrets-bin-dark.png">
    <img src="docs/assets/bench-secrets-bin-light.png" alt="A synthetic probe: credentials made by the training generator, planted in 2,000 windows of real binaries. Recall on 360 decidable credentials: PureByte secrets-bin 1.000 (95 % interval 0.990 to 1.000), strings plus regular expressions 0.803. Precision on 400 look-alike traps: 0.989 against 0.876." width="100%">
  </picture>
</p>

| Clean binaries: every finding is a false alarm | Files | Findings |
|---|---:|---|
| Python extension modules (scipy, scikit-learn) | 24 | 1 |
| Linux libraries and executables | 25 | 9, all in files that overlap the training corpus; 0 in the other 17 |
| Windows DLLs | 15 | 12, in 3 files |
| Files in 14 formats, archives opened (13,921 members) | 69 | 4, in 2 class files inside JARs |
| Extension modules held out before training | 60 | 2 |

There is no public benchmark for credentials in binaries yet, so these are our own exams, partly built from files
that cannot be redistributed. The probe of the chart is synthetic and in-distribution: its credentials, names and traps
come from the generator the model was trained with, in the layouts of its binary mode, and the `strings` + regex rival
was not built for them. It shows that the model learned what its generator makes (360 of 360, 95 % interval 0.990 to
1.000; 0.989 to 1.000 over six seeds), not how it does on real leaks: the clean-binary counts above are the harder
test: [benchmarks/RESULTS.md](benchmarks/RESULTS.md#secrets-bin).

### How we measure

- **Public data, fixed split.** Splits are decided before any measurement and computed from the dataset's own
  metadata, so anyone can rebuild them.
- **Criteria written before training, every verdict published.** What "good enough" means is written in the recipe
  before a model is trained, with a date and a prediction, and the factory freezes it by hash within a run. Every
  verdict is published, failures and changes included: `secrets-code` failed its two false-alarm guards on a
  clean-repository exam, `secrets-bin` its probe-recall floor on two of six seeds, and the criterion of `pii` passes at
  the operating point it was moved to after it was written, not at the one first written. All three were released;
  [benchmarks/RESULTS.md](benchmarks/RESULTS.md) gives the numbers and the reasons.
- **No leakage, or it is declared.** The data scripts check training data against every evaluation set, down to
  shared 64-byte blocks. Every overlap found in the released examples' data is measured and declared, never ignored:
  four CredData TEST repositories and nine clean-repository exam repositories for `secrets-code`, shared phrases with
  PIIMB for `pii`, and ten exam files for `secrets-bin`.
- **Several seeds.** Each recipe trains three to six models from different random seeds, and the spread is published
  next to the released file's own score.
- **Picked on held-out data, with one exception.** `pii` and `secrets-bin` were chosen on validation data. The
  `secrets-code` seed was chosen on the false-alarm exam code-fp-b, whose published count is therefore optimistic,
  among seeds within 0.01 CredData F1 of the ensemble, and their CredData TEST scores (0.788 to 0.799) were already
  known: the choice can move the headline F1 by at most 0.011, and the chosen one (0.797) is not the best.
- **Same harness where we can.** On CredData and the binary probe, competing tools run on the same files, with the
  same scoring code, on the same machine. On PIIMB, the other models' figures are the public leaderboard's: the same
  sentences and metric, their own inference code.
- **Uncertainty included.** F1 comes with a 95 % bootstrap interval over repositories.

Everything, with the scripts to reproduce it: [benchmarks/RESULTS.md](benchmarks/RESULTS.md).

## Train your own: Vibe Training

The training stack has its own repository, [purebyte-ai/purebyte-train](https://github.com/purebyte-ai/purebyte-train): the three examples are recipes in its
[specialists/](https://github.com/purebyte-ai/purebyte-train/blob/main/specialists/README.md) folder, and a new specialist is a new folder next to them.
**Vibe Training** is training one by describing what you want to an AI coding agent, the way vibe coding builds
software. The agent follows the golden path in
[its AGENTS.md](https://github.com/purebyte-ai/purebyte-train/blob/main/AGENTS.md#the-golden-path-vibe-training), and the training stack makes every step
checkable: leak checks against the evaluation sets, success criteria written before training, a control and a classic
rival to beat, and proof that the exported file gives the same answers in the runtime. A request can be this short:

```text
Train an AI Specialist that finds IBANs in CSV exports, and tell me honestly how good it is.
```

What it costs: the three examples were trained from scratch, with no pretrained body, on one consumer GPU. Each recipe
trains several models from different random seeds, plus a control with shuffled labels, and releases one:

| Example | Model | Each run | Training time on one AMD Radeon RX 6800 XT (16 GB) |
|---|---|---|---|
| `secrets-code` | `nano`: 4 blocks of width 256, 1.9 M parameters | 12,000 steps × 20 windows of 512 bytes | **24.5 minutes** for 3 seeds and the control, 4 at a time |
| `pii` | `nano_n15`: `nano` + n-gram tables, 8.3 M parameters | 12,000 steps × 16 windows | **7.5 minutes** per seed; 6 seeds and 2 controls, one at a time |
| `secrets-bin` | `tiny_n18`: 8 blocks + n-gram tables, 54 M parameters | 6,000 steps × 23 windows, 7 of them real negatives | **58 minutes** for 6 seeds and the control, 2 at a time |

All of it, with the bias sweeps and checks of each stage: **about 2.5 GPU-hours for the three examples**. The data
stages take under two minutes each; the exams on real data run on the CPU. What the machine needs:
[Compatible machines](#to-train-a-specialist).

The same path by hand, in five steps:

1. **Set up** the training stack: `git clone https://github.com/purebyte-ai/purebyte-train`; install PyTorch for your
   hardware ([pytorch.org](https://pytorch.org/get-started/locally/)); then, in the clone, `pip install -e "training[test]"`
   and `python -m pbtrain doctor`.
2. **Get the data** with the scripts in [data/](https://github.com/purebyte-ai/purebyte-train/blob/main/data/README.md): evaluation sets first, then corpora and catalogs,
   all from pinned public sources. Datasets stay outside git, in `$PUREBYTE_DATA`.
3. **Create the specialist**: `python -m pbtrain new-vertical NAME` (a *vertical* is the training stack's name for a
   specialist's data plugin), then its data generator, a classic rival and tests. To see every stage run first:
   `python data/build_smoke_data.py`, then the `pii` smoke recipe, trains and exports a tiny model on made-up data in
   about two minutes on a CPU, which `purebyte redact` runs (the training repository's Quick start).
4. **Train** with the success criteria written in the recipe beforehand:
   `python -m pbtrain run RECIPE --device auto --parallel 3`. The default device is the CPU; `auto` takes an NVIDIA or
   AMD GPU, or Apple silicon, when PyTorch sees one (only the AMD Radeon path has been run by the maintainers so far);
   on an AMD Radeon under Windows, `--device cuda --backend radeon` adds fused kernels. Three seeds inform, six decide
   (`secrets-code` 1.0.0 was released on three).
5. **Evaluate and release**: `python -m pbtrain eval RECIPE --record EXAM --metrics FILE` on real data, then
   `python -m pbtrain release RECIPE --public --semver X.Y.Z` writes a `.gguf` file that `purebyte` runs.

On a CPU, the trainer computes bit for bit what the code that trained the released examples computes on a CPU
(golden tests of 27 training paths). The released weights themselves were trained on a GPU, so a retraining lands
close to them, not bit for bit. A release records the code commit and every checksum. The guide: [docs/training.md](https://github.com/purebyte-ai/purebyte-train/blob/main/docs/training.md).

## Compatible machines

### To run a model

| | What it takes |
|---|---|
| **Operating systems** | Linux (the `pip` wheels need glibc 2.28 or newer; the release binaries are self-contained), macOS 11 or newer, Windows 10 or newer (x86-64) |
| **Processors** | x86-64, with the AVX2 kernel on CPUs that have AVX2 (most x86-64 CPUs of the last ten years); arm64 with the NEON kernel (Apple silicon, arm64 Linux); any other little-endian CPU with a C++17 compiler, through the portable kernel. Every kernel gives bit-identical results on a platform |
| **Memory** | 9 to 40 MB of peak memory to load one of the examples and answer a small input; about 180 to 240 MB during a large scan with 23 threads (fewer threads use less: 42 MB with 4) |
| **Disk** | The runtime (a few MB) and 1 to 29 MiB per model |
| **GPU** | Not used |
| **Network** | Outgoing: only `purebyte models pull`, to download a model ([offline installation](docs/install.md#models) works too); `purebyte serve` listens on loopback by default |
| **Python** | Optional: 3.8 or newer, for `pip install purebyte` and the Python API |
| **Containers** | `ghcr.io/purebyte-ai/purebyte`, for `linux/amd64` and `linux/arm64` |

Every change is built and tested on Linux (GCC and Clang), macOS (arm64 and x86-64) and Windows (MSVC and MinGW-w64),
and every release on Linux x86-64 and arm64 too. The figures in this README come from one desktop, an AMD Ryzen 9
5900X; `purebyte bench` measures yours.

### To train a specialist

| | What it takes | What we used |
|---|---|---|
| **Software** | Python 3.10 or newer and PyTorch 2.2 or newer (Python 3.14, or the `zstd` command, to unpack the binary exams) | Python 3.10 and PyTorch with ROCm, on Windows 10 |
| **GPU** | Optional. Tested on an AMD Radeon (ROCm, Windows); NVIDIA (CUDA), Linux ROCm and Apple silicon (Metal) run the same stock PyTorch code but have not been run by the maintainers yet. About 2 GB of free memory for a `nano` model and 4 GB for `tiny_n18`, at 16 windows per step (the released recipes use 20 and 23) ([hardware](https://github.com/purebyte-ai/purebyte-train/blob/main/training/docs/gpu.md)) | One AMD Radeon RX 6800 XT (16 GB), a 2020 gaming card: four `nano` jobs fit side by side |
| **CPU** | Enough on its own for the smoke recipes (minutes) and, slowly, for full ones; the CPU is the bit-exact reference of the trainer | AMD Ryzen 9 5900X, 12 cores |
| **Memory and disk** | A few GB of disk per specialist for its data; the examples use a 0.3 GiB code corpus, 0.95 GiB of CredData, 1.1 GiB of binaries and 3.6 GiB of public PII documents | 32 GB of RAM |

## Use it

### Command line

```bash
purebyte scan src/ config/                         # JSON findings; exit status 1 on an error
purebyte scan --staged                             # what you are about to commit
purebyte scan --git-diff origin/main               # a branch or a pull request: findings on added lines
purebyte scan --model secrets-bin dist/            # binaries and build artifacts, archives opened
purebyte scan --model my-model.gguf data/          # any model file you trained
purebyte scan --format sarif --out purebyte.sarif .
purebyte redact --model pii export.csv > export.redacted.csv
echo 'db_password = "Tr0ub4dor&3xyz"' | purebyte decide --model secrets-code -   # one small input: a decision
purebyte bench --model pii                         # latency and throughput on your machine
```

Every command and option: [docs/cli.md](docs/cli.md).

### Local HTTP API

Load a model once and answer in milliseconds, from any language:

```bash
purebyte serve --model pii                         # http://127.0.0.1:8421, loopback only
curl -s http://127.0.0.1:8421/v1/redact \
  -H 'Content-Type: application/json' \
  -d '{"text": "Call Laura Gómez on +34 612 345 678"}'
```

The server refuses requests from other origins, never writes to disk and logs no content: [docs/api.md](docs/api.md).

### Python

```python
import purebyte

result = purebyte.scan("settings.py", model="secrets-code")
for finding in result.findings:
    print(finding.file, finding.line, finding.col, finding.snippet_masked)

redaction = purebyte.redact(open("ticket.txt", "rb").read(), model="pii")
print(redaction.output.decode())
```

Standard library only (`ctypes`), the same results as the CLI: [docs/api.md](docs/api.md#python).

### C

The engine is a C library with a stable ABI (`purebyte/pb.h`): load a model once, share it between threads, and call
it from any language with a C foreign-function interface: [docs/api.md](docs/api.md#c-api).

### Integrations

| Where | What you get |
|---|---|
| [pre-commit](integrations/pre-commit/) | Staged changes scanned in about a second; the commit stops on a finding outside test and example paths |
| [GitHub Action](integrations/github-action/) | Findings as code scanning alerts, inline in the pull request diff |
| [GitLab CI](integrations/gitlab-ci/) | A ready-made job with model caching and a SARIF artifact |
| [Docker](integrations/docker/) | The runtime in a slim image; models pulled at run time |
| [VS Code](integrations/vscode/) | Tasks for the current file, the staged changes or the workspace |

```yaml
# one step of a workflow; the whole workflow, with its permissions: integrations/github-action
- uses: purebyte-ai/purebyte/integrations/github-action@v1
  with:
    model: secrets-code
```

Using PureByte? Show it with the [badge](integrations/README.md#scanned-by-purebyte-badge).

## Configure

There is no configuration file to write. A model file carries its own defaults (window, operating point,
post-processing), and everything else is a command-line option, an environment variable, or a `.purebyteignore` file
next to your data.

| To change | Use | Default |
|---|---|---|
| Where models are stored | `PUREBYTE_HOME=/path/to/models` | Linux `$XDG_DATA_HOME/purebyte/models` (by default `~/.local/share/purebyte/models`), macOS `~/Library/Application Support/purebyte/models`, Windows `%LOCALAPPDATA%\purebyte\models` |
| Where models come from | `PUREBYTE_CATALOG=catalog.json`, for example with `file://` URLs of an internal mirror | The catalog built into the binary, with the SHA-256 of every file |
| The model | `--model pii`, `--model secrets-code@1.0.0`, or the path of any `.gguf` file | `secrets-code` (`redact` needs one) |
| Precision against recall | `--bias X` (lower is more precise, higher finds more), `--type-bias TYPE=X`, `--min-confidence X` | The operating point stored in the model |
| Ensembles | `--model secrets-code-ensemble`, `--ensemble FILE`, `--votes K`, `--no-ensemble` | One model |
| Threads | `--threads N`, `--intra-threads N` | Hardware threads (logical CPUs) minus one; up to 8 threads per window |
| Post-processing | `--profile none`, `secrets-code`, `secrets-binary` or `redact` | The profile the model file declares |
| Paths to skip | `--exclude GLOB` and `.purebyteignore` files (`.gitignore`-style patterns), `--tests no` | `secrets-code`: code, configuration and text files, skipping `.git`, `node_modules`, `vendor`, `dist`, `build`, `target`, `.next`, `__pycache__`, `.venv` and `venv`; other profiles: every file |
| Findings to accept | A `purebyte:allow` comment on the line (`secrets-code` profile) | None |
| When a scan fails | `--strict`: warnings fail too | Errors fail (exit status 1); findings in test, example and documentation paths are warnings |
| Binaries | `--all-bytes` (not only printable strings), `--no-archives`, `--prefilter` (faster, not exact) | Printable strings only; zip, gzip and tar opened |
| Output | `--format json`, `jsonl` or `sarif`; `--out FILE`; `--reveal` prints the values in clear | JSON on standard output, values masked |
| The local server | `--host`, `--port`, `--token-file FILE` or `PUREBYTE_SERVE_TOKEN`, `--archives`, `--max-body-mb N` | `127.0.0.1:8421`, no token, archives off, 80 MiB |

A `.purebyteignore` at the top of the scanned folder (or of the repository, for `--staged` and `--git-diff`):

```text
# generated and vendored code
generated/
/vendor
*.min.js
docs/examples/**
```

A pre-commit hook with the `secrets-code` example, in `.pre-commit-config.yaml`:

```yaml
repos:
  - repo: https://github.com/purebyte-ai/purebyte
    rev: v1.0.0
    hooks:
      - id: purebyte
```

Every option of every command: [docs/cli.md](docs/cli.md). The HTTP API: [docs/api.md](docs/api.md). What a model file
lets you change at run time: [docs/customize.md](docs/customize.md).

## The future of PureByte: a marketplace of AI Specialists

Today there are three example specialists, and we trained them. The plan, still in design, is a place where anyone
can publish the specialists they train on this architecture, each one a `.gguf` file with its model card, its recipe
and its exam results, listed only after the checks ours go through (leak checks, criteria written before training,
results reproduced from the file), and where you can post the specialist you need. Whatever you take from it is a file
that runs on your own machines, so your data never leaves them. Details and how to take part early:
[docs/roadmap.md](docs/roadmap.md).

### Next releases

- New specialists for small decisions: screening prompts before they reach a hosted model, classifying log lines and
  configuration values, identifying file types and encodings, extracting fields by question.
- A verified mode in which a larger model reviews the candidates of a fast one.
- Better examples: higher recall for `secrets-code`, a public evaluation for `secrets-bin`, typed labels and more
  languages for `pii`, calibrated confidence.
- **Vibe Training as a service**: describe the specialist you need and get it trained, evaluated and delivered as a
  file for this runtime.

Details: [docs/roadmap.md](docs/roadmap.md).

## Honest limits

What the architecture and the examples do not do, next to what they do:

- **One job per model.** A specialist answers the question it was trained for. Summaries, translations, open
  questions and anything that needs broad knowledge of the world are jobs for large language models, and a new
  question means training a new specialist.
- **Large inputs cost time.** A neural network reads every byte: about 130 KB/s on a 12-core desktop for the small
  examples. Pattern matchers are much faster per byte (tens of times on our machine) and a good first layer where a pattern
  exists.
- **Confidence is not calibrated yet.** It ranks answers of one model; a 0.9 does not mean "right nine times in ten".
- **The `secrets-code` example still misses about one credential in four** on CredData as distributed, and two in
  five on its original values: passwords that are ordinary words, tokens without a telling context, and salts, nonces
  and UUIDs (reported only under an unequivocal credential name, by design). On
  ordinary repositories it raises warnings on credential-shaped test data, and some errors on integrity hashes and
  demo keys. Use it next to your pattern scanner.
- **The `pii` example is not first on PIIMB.** It is weakest on protocol traces with a labeling policy of their own
  and on EU legal texts. It masks one type, `PII`, with the benchmark's policy (dates and identifiers count), and does
  not by itself make a dataset compliant with any law.
- **None of the three examples met every pre-registered criterion as first written.** `secrets-code` failed its
  false-alarm guards on a clean-repository exam, `secrets-bin` its probe-recall floor on two of six seeds, and the
  criterion of `pii` was moved to another operating point after it was written; the numbers and the reasons are in
  [benchmarks/RESULTS.md](benchmarks/RESULTS.md). Nine of the 36 repositories of `secrets-code`'s
  clean-repository exams overlap its training data, so its counts there can be off in either direction; the same file
  gives every set with and without them.
- **The training stack builds span models today.** The runtime runs every head of the format; the trainer builds the
  span heads the three examples use, and the other heads are on the [roadmap](docs/roadmap.md).
- **Every speed comes from one machine**: an x86-64 desktop with AVX2. None comes yet from an arm64 CPU, the NEON
  kernel or an embedded board; the NEON kernel is checked for exactness in CI, not timed.
- **The `secrets-bin` example is tuned for few false alarms.** A single weak credential alone in a small binary can
  go unflagged, and our binary exams are partly built from files that cannot be redistributed.

## FAQ

<details>
<summary><b>Is PureByte a secret scanner?</b></summary>

No. PureByte is an architecture, a runtime and a training stack for tiny byte-level models. Finding credentials and
masking personal data are the first three examples we trained on it, chosen because two of those jobs have public
benchmarks and strong tools to compare with. The same runtime runs any specialist trained for another decision.
</details>

<details>
<summary><b>Is it a large language model?</b></summary>

No. It generates no text, has no tokenizer and weighs megabytes. Each specialist is a small neural network that reads
bytes and answers with a decision, a score or a byte range: closer to a very well-read regular expression that
understands context than to a chatbot.
</details>

<details>
<summary><b>Can it hallucinate?</b></summary>

It can be wrong, but it cannot invent anything: every answer is a label declared in the model file, a score, or a
range of bytes of your input, and a redacted copy is checked to be your input byte for byte outside the masked spans.
</details>

<details>
<summary><b>What can I build with it?</b></summary>

Any job that is a decision about bytes: classify an input, score it, label several properties, find and type spans,
extract a value by question, map each byte, or redact. The format and the runtime support all of these heads today;
the three examples use spans, and the training stack trains span models today, the other heads next. See [What you can build](#what-you-can-build).
</details>

<details>
<summary><b>Does it need a GPU?</b></summary>

Not to run: x86-64 (with AVX2 when available) on Linux, macOS and Windows, arm64 on Linux and macOS, and a portable
build for other little-endian CPUs. To train, a GPU is faster (one consumer card is enough), and a CPU works too.
</details>

<details>
<summary><b>How can a specialist on a CPU answer before Laya on a GPU?</b></summary>

It does far less work, on a narrower job. The model body of a specialist (without its lookup tables) has 1.9 M to 3.8 M
parameters, against Laya's 421 M; the rest of a large specialist is lookup tables, which cost three row reads per
byte. It reads a window in one linear pass, the CPU's threads share the work of that one window, and the kernels are
written for the CPU's vector units. Its question is also fixed when it is trained: there is no question text to encode
next to the input and no options to compare, which Laya does on every request. Laya's GPU time (33 to 40 ms on a T4)
is its own published figure; the conditions of ours are in
[docs/performance.md](docs/performance.md#compared-with-laya-and-jev).
</details>

<details>
<summary><b>Is it better than Laya or Jev?</b></summary>

For a decision it was trained for, it answers in less time, is smaller, keeps the data on your machine and runs on a
CPU; see [the comparison](#purebyte-laya-and-typesafe-jev). Laya and Jev answer any question you write at request time, which a
specialist does not: for a new question you train a new specialist. They have not been measured on the examples'
benchmarks, and PureByte has not been measured on theirs.
</details>

<details>
<summary><b>Does my data leave the machine?</b></summary>

Never. Models run inside the process, there is no telemetry, and the only outgoing network access of the runtime is
the model download you start with `purebyte models pull` (`purebyte serve` listens on loopback by default). The credential examples never test credentials against their
providers, and detected values are masked in every output unless you ask to reveal them:
[docs/privacy.md](docs/privacy.md).
</details>

<details>
<summary><b>Can it handle long files and long contexts?</b></summary>

Long files, yes: they are read in overlapping windows, so length only costs time, linearly. Long context, not yet:
each decision of the released examples sees 512 bytes. The format has a stream context for inputs of any length, but
no released model or recipe uses it yet: [Long inputs](#long-inputs-and-longer-windows) and the
[roadmap](docs/roadmap.md).
</details>

<details>
<summary><b>Can I train my own specialist, and what does it cost?</b></summary>

Yes, with the training stack, the examples' recipes and the data scripts in [purebyte-train](https://github.com/purebyte-ai/purebyte-train), by hand or
through an AI coding agent ([Vibe Training](#train-your-own-vibe-training)), on a CPU or a GPU (tested on an AMD
Radeon under Windows; NVIDIA, Linux ROCm and Apple silicon run the same stock PyTorch code but have not been run by
the maintainers yet). It trains span models today. The `secrets-code` recipe trains three seeds from
scratch in 25 minutes on one consumer GPU, and the three examples took about 2.5 GPU-hours in all; on a CPU, the `pii` smoke
recipe trains and exports a tiny model on made-up data in about two minutes.
</details>

<details>
<summary><b>Why ternary weights? Don't they cost accuracy?</b></summary>

They cost accuracy and save space. The models are trained with them from the first step (quantization-aware
training), so nothing is lost in a conversion afterwards, and they keep the files small: 2.25 bits per weight, scales
included, against 16 or 32. In our experiments on 24 clean binaries, a float control made 2.4 times fewer false alarms
than the same ternary model, while adding the n-gram memory cut the ternary model's 13 times; a float model with the
memory was not trained, so ternary plus memory is what we ship, not a proven optimum
([benchmarks/RESULTS.md](benchmarks/RESULTS.md#design-experiments)). Every number of the examples is of the ternary
models.
</details>

<details>
<summary><b>Why state-space blocks and not attention?</b></summary>

They read a sequence in one linear pass with a fixed-size state, which suits long, byte-level inputs, streaming and
CPUs. The runtime also has an attention block for models that need one; in our experiments one late attention layer
lowered the bits per byte of unseen code by 15 % but raised the false alarms of both credential examples
([benchmarks/RESULTS.md](benchmarks/RESULTS.md#design-experiments)).
</details>

<details>
<summary><b>Should I replace gitleaks or trufflehog with the <code>secrets-code</code> example?</b></summary>

Run both. Pattern scanners are extremely fast and precise on known key formats; `secrets-code` adds what has no fixed
format (passwords, URL credentials, generic keys), found from their context. On CredData it finds 3.5 times as many
labeled credentials as gitleaks (2.9 times on the original values), and it still misses one in four (two in five on the
original values).
</details>

<details>
<summary><b>How do I silence a false positive?</b></summary>

Add `purebyte:allow` in a comment on its line, or leave a path out with `--exclude` or a `.purebyteignore` file, which
takes `.gitignore`-style patterns: [docs/cli.md](docs/cli.md#ignoring-findings). In CI, alerts dismissed in GitHub's
code scanning view stay dismissed.
</details>

<details>
<summary><b>Which languages does the <code>pii</code> example read?</b></summary>

It learned from documents in 23 languages; bytes are bytes, so it reads any encoding, but quality is measured on those.
On the benchmark's multilingual task it scores F2 0.950.
</details>

<details>
<summary><b>Can I use it commercially?</b></summary>

Yes. The code is Apache-2.0, and the released example weights are free to use for any purpose, commercial use
included, on hardware you own or control, including cloud instances and CI runners that run on your behalf, under
the [PureByte Model License](MODEL_LICENSE.md). It does not allow
redistributing the weights, building models from them or offering a hosted service whose main value is the model
itself. The outputs are
yours, and so is any model you train yourself with the training code.
</details>

<details>
<summary><b>How do I cite it?</b></summary>

With [CITATION.cff](CITATION.cff), or the BibTeX [below](#citation).
</details>

More answers, including confidence, votes and operating points: [docs/faq.md](docs/faq.md).

## Links and community

| Where | Link |
|---|---|
| Website | [purebyte.ai](https://purebyte.ai) |
| Research paper | [PureByte: SLMs Are the Future](https://doi.org/10.5281/zenodo.23020056) (Zenodo) |
| Code, issues, discussions | [github.com/purebyte-ai](https://github.com/purebyte-ai) |
| Training stack | [github.com/purebyte-ai/purebyte-train](https://github.com/purebyte-ai/purebyte-train) |
| Models | The releases of this repository ([catalog](models/README.md)); mirror: [huggingface.co/purebyte](https://huggingface.co/purebyte) |
| Python package | [pypi.org/project/purebyte](https://pypi.org/project/purebyte/) |
| Container image | [ghcr.io/purebyte-ai/purebyte](https://github.com/purebyte-ai/purebyte/pkgs/container/purebyte) · [Docker Hub](https://hub.docker.com/r/purebyte/purebyte) |
| X | [@purebyteai](https://x.com/purebyteai) |
| YouTube | [@purebyteai](https://www.youtube.com/@purebyteai) |
| Instagram | [@purebyte.ai](https://www.instagram.com/purebyte.ai/) |
| TikTok | [@purebyte](https://www.tiktok.com/@purebyte) |
| Reddit | [u/purebyteai](https://www.reddit.com/user/purebyteai/) |
| Blog / Medium | [sirventai.medium.com](https://sirventai.medium.com) |
| E-mail | [pablo@purebyte.ai](mailto:pablo@purebyte.ai) |

- **Bugs, false positives and misses**: [issues](https://github.com/purebyte-ai/purebyte/issues), with masked snippets
  only, never a real secret or real personal data.
- **Questions and ideas**: [discussions](https://github.com/purebyte-ai/purebyte/discussions).
- **A specialist for your own data, trained for you, or other license terms**:
  [pablo@purebyte.ai](mailto:pablo@purebyte.ai).

## Contributing

New specialists, kernels, blocks and heads, integrations, bug reports, false positives and misses, and documentation
are all welcome. Start with [CONTRIBUTING.md](CONTRIBUTING.md); AI coding agents should read [AGENTS.md](AGENTS.md).
Please follow the [Code of Conduct](CODE_OF_CONDUCT.md). Found a vulnerability? Please report it privately:
[SECURITY.md](SECURITY.md).

## License

- **Code** (engine, CLI, HTTP server, Python bindings, specification, reference implementation, integrations,
  documentation): [Apache License 2.0](LICENSE). The training stack, recipes and data scripts, in
  [purebyte-train](https://github.com/purebyte-ai/purebyte-train), are under the same license.
- **Model weights** (the released `.gguf` example specialists): the [PureByte Model License](MODEL_LICENSE.md): free
  use for any purpose, commercial use included; no redistribution, no models built from the weights, no hosted service
  whose main value is the model itself. The outputs are yours, and so is any model you train yourself with the training code.
- **Datasets** fetched by the scripts in [data/](https://github.com/purebyte-ai/purebyte-train/blob/main/data/README.md) keep their own licenses, recorded in their manifests.
- The PureByte name and logo are not licensed for use beyond referring to this project.

## Author

PureByte was created and built by **Pablo Sirvent Jiménez**:

- **Websites:** [sirvent.ai](https://sirvent.ai) · [pablosirvent.com](https://pablosirvent.com)
- **GitHub:** [@sirventai](https://github.com/sirventai)
- **X / Twitter:** [@sirventai](https://x.com/sirventai)
- **Blog:** [sirventai.medium.com](https://sirventai.medium.com)
- **Contact:** [pablo@purebyte.ai](mailto:pablo@purebyte.ai)

## Citation

If PureByte or the paper helps your research, please cite:

```bibtex
@article{purebyte2026paper,
  title   = {PureByte: SLMs Are the Future},
  author  = {Sirvent Jiménez, Pablo},
  year    = {2026},
  doi     = {10.5281/zenodo.23020056},
  url     = {https://doi.org/10.5281/zenodo.23020056}
}

@software{purebyte,
  title   = {PureByte: an open runtime for tiny byte-level AI Specialists},
  author  = {Sirvent Jiménez, Pablo},
  version = {1.0.0},
  year    = {2026},
  url     = {https://github.com/purebyte-ai/purebyte}
}
```

---

<p align="center">
  <sub>Built by <a href="https://sirvent.ai">Pablo Sirvent Jiménez</a> (<a href="https://github.com/sirventai">@sirventai</a>) · <a href="https://purebyte.ai">purebyte.ai</a></sub>
</p>
