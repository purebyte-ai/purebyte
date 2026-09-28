# Roadmap

Where PureByte is going, in the order we expect to get there. Nothing here has a date yet, and every quality claim
will be measured and published in [benchmarks/RESULTS.md](../benchmarks/RESULTS.md) before a release makes it.
Suggestions are welcome as feature requests.

## 1.0: the architecture, the runtime, the training stack and three examples

- **The architecture**: byte embedding, hashed n-gram memory stored at 4 bits, ternary Mamba-2-style state-space
  blocks, and heads that decide, score, label bytes and map them, with constrained decoding. Specified in
  [spec/FORMAT.md](../spec/FORMAT.md) and described in [architecture.md](architecture.md).
- **The runtime**: one C++17 engine that runs any model declared by its GGUF file, with the same bits from the
  scalar, AVX2 and NEON kernels and at any thread count on a platform; threads that share one window for low latency; the `purebyte` CLI, a local HTTP
  API, Python bindings, a C API, SARIF output and redaction by copy.
- **The training stack**, in its own repository, [purebyte-train](https://github.com/purebyte-ai/purebyte-train):
  `pbtrain`, with data generators grounded in real bytes, leak checks, criteria written before training, multi-seed
  runs and the proof that the runtime gives the same answers, by hand or through **Vibe Training**
  ([training.md](https://github.com/purebyte-ai/purebyte-train/blob/main/docs/training.md)). It trains span-finding
  specialists, on a CPU or an AMD Radeon GPU (tested on Windows); NVIDIA, Linux ROCm and Apple silicon GPUs use the
  same stock PyTorch code but have not been run by the maintainers yet.
- **Three example specialists**: two jobs with public benchmarks and strong incumbents, `pii` (personal data masked
  in any text, measured on the PII Masking Benchmark) and `secrets-code` (credentials in code, measured on CredData),
  and one with no public benchmark yet, `secrets-bin` (credentials inside compiled binaries, measured on our own
  exams).
- Integrations: pre-commit, GitHub Action with code scanning, GitLab CI, Docker, VS Code tasks.

## Next: more specialists, for small decisions

Many useful questions are about a few hundred bytes: one line, one log event, one HTTP request, one tool call of an
AI agent. For those, latency and privacy matter more than throughput, and the format and the runtime already have the
heads they need (`choice`, `multilabel`, `score`, `ordinal`, extraction by question). The training stack does not
train them yet: today it builds span-finding specialists, and training decisions, labels, scores, extraction and
per-byte maps is the first step of this direction. Directions we are exploring:

- Screening prompts before they reach a hosted model: prompt injection, secrets and personal data in the prompt.
- Classifying log lines and configuration values, and extracting the fields a project needs.
- Identifying file types, programming languages and text encodings from content alone.
- Log anomalies and log parsing as typed spans.

## Next: the architecture and the runtime

- **Verified mode**: a tiny model reads everything and a larger model reviews only the few candidates it flags.
- **Calibrated confidence**, so that a 0.9 means nine out of ten.
- **Models trained for the stream context**, to read inputs of any length with a fixed-size state.
- **Long-range recall**: an exact memory next to the state (a sparse attention block that attends to a local window
  plus a few selected positions, or a small bounded cache of exact bytes), so that a model reading a megabyte in one
  stream can also use a detail from its beginning. Claimed only after it is measured on a real long-context benchmark,
  not on synthetic retrieval alone.
- Performance figures from more machines (Linux, macOS, arm64 CPUs) and optimized kernels for more CPUs, always
  checked against the reference path.
- **A row count per n-gram order** in a future format version. Today every order's table has the same number of rows,
  so in `secrets-bin` the order-2 table stores 196,608 rows that no bigram can reach (7.1 MB of its 30.2 MB file); an
  order-2 table sized to its 65,536 bigrams would remove them.

## Next: better examples

- **Higher recall for `secrets-code`**, starting with the weak spots measured on CredData, and models trained on the
  public corpus rebuilt without any CredData repository.
- **A public evaluation for `secrets-bin`**, built only from public binaries.
- **Typed markers for `pii`** (`[EMAIL_1]`, `[IBAN_1]`...), where the kind of personal data matters, from its
  experimental typed recipe.

## Coming: a marketplace of AI Specialists, and Vibe Training as a service

- **A community marketplace**: anyone publishes the specialists they train on this architecture, each verified with
  the checks ours go through (leak checks, criteria written before training and every verdict published, failures
  included, results reproduced from the file), and
  creators choose their terms and earn when their specialists are used. Requests let users post the specialist they
  need and trainers compete to deliver it. Whatever you take from it runs on your own machines.
- **Vibe Training as a service**: describe the AI Specialist you need, in plain words, and get it trained, evaluated
  honestly and delivered as a file that this runtime already knows how to run.

Both are in design. To take part early: [pablo@purebyte.ai](mailto:pablo@purebyte.ai).
