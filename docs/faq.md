# Frequently asked questions

- **The basics:** [What is PureByte?](#what-is-purebyte) · [A secret scanner?](#is-purebyte-a-secret-scanner) ·
  [AI Specialists](#what-is-an-ai-specialist) · [What can I build?](#what-can-i-build-with-it) ·
  [Is it an LLM?](#is-it-a-large-language-model) · [Can it hallucinate?](#can-it-hallucinate) ·
  [Why bytes?](#why-bytes-and-not-tokens) · [Why so small?](#why-are-the-models-so-small) ·
  [Why so fast?](#why-is-it-so-fast-on-a-cpu) · [GPU?](#does-it-need-a-gpu) ·
  [Platforms](#which-platforms-does-it-run-on)
- **Compared with others:** [Laya and Jev](#how-does-it-compare-with-laya-and-typesafe-jev) ·
  [Large language models](#why-not-just-ask-a-large-language-model) ·
  [gitleaks and trufflehog](#how-does-it-compare-with-gitleaks-and-trufflehog) ·
  [PII tools](#how-does-pii-compare-with-other-pii-tools)
- **Privacy:** [Does it send my data anywhere?](#does-it-send-my-data-anywhere) ·
  [Does it test credentials?](#does-it-check-whether-a-credential-works) ·
  [Does it print secrets?](#does-it-print-the-secrets-it-finds) · [The redaction map](#is-the-redaction-map-safe-to-keep)
- **The credential examples:** [Replace my scanner?](#should-i-replace-my-current-scanner) ·
  [Misses](#why-does-it-still-miss-credentials) · [False alarms](#how-do-i-silence-a-false-positive) ·
  [Reporting errors](#it-flagged-something-harmless-or-missed-a-secret-what-now) · [Speed in CI](#is-it-fast-enough-for-ci) ·
  [Only what changed](#can-it-scan-only-what-changed) · [Binaries and archives](#what-can-secrets-bin-read)
- **The personal-data example:** [What counts as PII?](#what-does-pii-mask) · [Languages](#which-languages-does-pii-read) ·
  [Compliance](#does-pii-make-my-data-compliant) · [LLM prompts](#can-i-mask-personal-data-before-it-reaches-an-llm)
- **The model:** [Ternary weights](#why-ternary-weights) · [State-space blocks](#why-state-space-blocks-and-not-attention) ·
  [N-gram memory](#what-is-the-n-gram-memory) · [Long inputs](#can-it-handle-long-files-and-long-contexts) ·
  [Determinism](#will-i-get-the-same-results-on-another-machine) · [Confidence](#what-does-confidence-mean) ·
  [Votes](#what-are-votes) · [Operating points](#what-does---bias-do)
- **Training:** [Train my own](#can-i-train-my-own-specialist) · [Vibe Training](#what-is-vibe-training) ·
  [What it costs](#what-does-training-cost) · [Which GPU](#which-gpu-do-i-need-to-train) ·
  [Reproducing the releases](#can-i-reproduce-the-released-models)
- **Licensing:** [Open source?](#is-purebyte-open-source) · [Commercial use](#can-i-use-it-commercially) ·
  [A specialist for my data](#can-you-build-a-specialist-for-my-data)

## The basics

### What is PureByte?

An architecture, a runtime and a training stack for tiny byte-level models, which we call AI Specialists. The models
read raw bytes and answer with decisions, scores, labels and byte ranges instead of text; the runtime runs any of them
on a CPU, with the same bits from every kernel and thread count of a platform; the training stack makes new ones. Three
example specialists come with the first release: they mask personal data (`pii`), find credentials in source code
(`secrets-code`) and find credentials inside binaries (`secrets-bin`). See the [README](../README.md) and
[how-it-works.md](how-it-works.md).

### Is PureByte a secret scanner?

No. Finding credentials and masking personal data are the first three examples trained on the architecture: two
jobs with public benchmarks and strong incumbents (PIIMB, CredData), and one, credentials inside binaries, that has no
public benchmark yet. The same runtime runs any model in the format, for another decision: classifying prompts or log
lines, scoring events, extracting fields, identifying file formats. The training stack builds span-finding
specialists today, and decisions, labels, scores and maps next ([roadmap](roadmap.md)).

### What is an AI Specialist?

A tiny model (1 to 29 MiB for the examples) that does one thing very well, on a CPU, without a cloud service and
without generating text. Compared with a general-purpose language model, a specialist is smaller, faster,
cheaper, private and measurable: its answers are decisions and byte ranges, so it can be scored exactly on public
data.

### What can I build with it?

Any job that is a decision about bytes. A model can answer with one decision among several (`choice`), several labels
at once (`multilabel`), a score or a grade (`score`, `ordinal`), typed byte ranges (`tag`), a value found by asking for
it (query templates), or one label per byte (`byte_map`), and the redaction profile turns spans into a verified
redacted copy. The format and the runtime support all of these today; the three examples use spans, and the training
stack trains span models only for now. See [customize.md](customize.md) and the
[README](../README.md#what-you-can-build).

### Is it a large language model?

No. It does not generate text, it has no tokenizer, and its models weigh megabytes. Each specialist does one job:
closer to a very well-read regular expression that understands context than to a chatbot.

### Can it hallucinate?

It can be wrong: it can miss something or flag something harmless. It cannot invent anything, because every answer is a
choice among options declared in the model file, a score, or a range of bytes that exist in your input. A redacted copy
is checked, before it is returned, to be your input byte for byte outside the masked spans.

### Why bytes and not tokens?

Tokenizers are built for natural language. Keys, base64, hex, minified code, UTF-16 and machine code break into
arbitrary pieces, and positions have to be mapped back approximately. A byte model sees exactly what is there and
answers with exact offsets. More in [how-it-works.md](how-it-works.md#why-bytes).

### Why are the models so small?

Four reasons: the model body of each specialist, without its n-gram tables, is small (1.9 M to 3.8 M parameters) because
it learns one task instead of a language; its large weight matrices are ternary (−1, 0 or +1, 2.25 bits per weight with
their scales); the input alphabet is only 256 byte values, so there is no large vocabulary to embed; and the larger
specialists keep most of their knowledge in n-gram lookup tables stored at 4 bits per value.

### Why is it so fast on a CPU?

It does little work per decision, and it does it the way CPUs like. Without its n-gram tables, the model body of a
specialist has 1.9 M to 3.8 M parameters; the tables cost three row reads per byte, however large they are;
state-space blocks read a window in one linear pass with a fixed-size state; the kernels are written for AVX2 and NEON
vector units; and the threads of the CPU share the work of a single window, so one 64-byte window of `secrets-code`
takes 0.82 ms with 8 threads instead of 2.97 ms on one thread, on a 12-core desktop CPU. Details and conditions in
[performance.md](performance.md).

### Does it need a GPU?

Not to run. It runs on ordinary CPUs: release binaries for x86-64 (with AVX2 when available) on Linux, macOS and
Windows and for arm64 on Linux and macOS, and a portable C++17 build for any other little-endian CPU. Training a
specialist is much faster on a GPU (one consumer card is enough), but a CPU can do it too.

### Which platforms does it run on?

x86-64 on Linux, macOS and Windows, and arm64 on Linux and macOS (Apple silicon included), as a self-contained
binary, a Python package (`pip install purebyte`, Python 3.8 or newer), a Docker image or a library with a C API; other
little-endian CPUs build from source. The kernel is chosen at run time: AVX2 on x86-64 CPUs that have it, NEON on
arm64, a portable scalar kernel elsewhere, all bit-identical on a platform. See [install.md](install.md).

## Compared with others

### How does it compare with Laya and TypeSafe Jev?

They are non-generative decision models: you write a question and its options, and they answer in one pass. For a
small decision that a specialist was trained for, PureByte takes far less time and runs on a CPU: on the same idle
desktop CPU, one decision about a line of code took 373 ms with Laya (421 M parameters, 12 threads, a three-option
question whose text is part of its input) and 2.81 ms with `secrets-code` (1.9 M parameters, one 256-byte window run
by the engine, 8 threads). That compares a small single-task model with a large general one, not the architectures
alone, and not quality. Laya's own figure on a T4 GPU is 33 to 40 ms, and Jev's hosted API is quoted at 236 to 276 ms
by third parties that Laya's model card cites. PureByte's files are 1 to 29 MiB against Laya's 808 MB, its specialists
are trained from scratch on one consumer GPU, and nothing leaves your machine. What Laya and Jev do that a specialist
does not: answer any question written at request time, without training. Conditions:
[performance.md](performance.md#compared-with-laya-and-jev).

### Why not just ask a large language model?

You can, and for open questions you should. For finding and masking credentials and personal data at scale, a
specialist is hundreds of times faster per decision (on the same desktop CPU, a 4-billion-parameter model labeled 15
to 19 candidate credentials a minute, on a machine busy with other work, against a few milliseconds per decision for
a specialist: [performance.md](performance.md#compared-with-a-general-language-model)), runs where the data is,
answers with exact byte ranges instead of free text to parse, cannot invent a value, gives the same answer every time,
and can be scored exactly on public benchmarks.

### How does it compare with gitleaks and trufflehog?

On the public CredData benchmark (164 TEST repositories, files up to 4 MB), `secrets-code` 1.0.0 finds about three and
a half times as many labeled credentials as gitleaks at a precision close to it: recall 0.729 against 0.207, precision
0.879 against 0.905, F1 0.797 against 0.337. The difference is in what has no fixed format: passwords (recall 0.88
against 0.06), URL credentials (0.99 against 0.01), generic keys (0.82 against 0.49). It learned partly from labeled
lines of the benchmark's DEV half (never from TEST), whose credential values CredData replaces with random strings:
with the original values restored, its recall is 0.583 and its F1 0.694, against 0.201 and 0.329 for gitleaks, 2.9 times
as many credentials found, and it misses more passwords that are ordinary words
([details](../benchmarks/RESULTS.md#on-the-original-values)). trufflehog, which is
built around verified detectors, scores 0.047 there without verification. All tools were run by us on the same files;
the details, confidence intervals and caveats are in [benchmarks/RESULTS.md](../benchmarks/RESULTS.md).

### How does `pii` compare with other PII tools?

On the PII Masking Benchmark, `pii` (8.3 M parameters) scores a mean F2 of 0.769: above OpenAI's Privacy Filter (0.662,
1.4 B parameters, about 50 M active) and Presidio with spaCy (0.586), and ahead of 23 of the 35 models on the public
leaderboard. Twelve models score higher, all with at least 68 M parameters; the first scores 0.836 with 1.4 B. The
benchmark's policy masks every kind of personal data as one type, as `pii` does, so a model with a narrower, typed
policy scores lower there by construction. Every figure and its conditions:
[benchmarks/RESULTS.md](../benchmarks/RESULTS.md#pii-on-the-pii-masking-benchmark), and the whole leaderboard:
[Leaderboard context](../benchmarks/RESULTS.md#leaderboard-context).

## Privacy

### Does it send my data anywhere?

No. Models run inside the process, there is no telemetry, and the only outgoing network access of the runtime is the
model download you start with `purebyte models pull`, which checks every file's SHA-256; `purebyte serve` listens on
loopback by default and answers only the requests sent to it. Details: [privacy.md](privacy.md).

### Does it check whether a credential works?

Never. Some scanners try credentials against the provider's API to confirm them, which sends your secret to a third
party. PureByte only looks at bytes.

### Does it print the secrets it finds?

Not by default: findings show a masked snippet such as `ghp_************8D`, and a finding that covers only part of a
token is masked with the whole token. The value is printed only when you ask for it explicitly.

### Is the redaction map safe to keep?

Treat it as the secret it is: `--map` writes the original values, so that `purebyte redact --restore` can put them
back. Without the map, a redacted copy holds none of the masked values, and its report holds neither the values nor a
hash of them. The CLI writes the map readable by its owner only on Linux and macOS.

## The credential examples

### Should I replace my current scanner?

We suggest running both. Pattern-based scanners are extremely fast and precise on known key formats. `secrets-code`
adds credentials that have no fixed pattern, recognized from their context, and it reads UTF-16 and base64 layers. It
still misses a share of real credentials (one in four on CredData as distributed, two in five on its original values),
so it is a second layer, not a guarantee.

### Why does it still miss credentials?

Some credentials cannot be told apart from ordinary values by looking at them: a short password that is an ordinary
word, a value with no name next to it, an identifier that a dataset labels as a credential. Which classes the released
model misses most is measured and published ([known weak spots](../benchmarks/RESULTS.md#known-weak-spots)), and
better recall is on the [roadmap](roadmap.md#next-better-examples).

### How do I silence a false positive?

Add `purebyte:allow` in a comment on its line, or leave a path out with `--exclude` or a `.purebyteignore` file, which
takes `.gitignore`-style patterns: [cli.md](cli.md#ignoring-findings). In CI, alerts dismissed in GitHub's code
scanning view stay dismissed.

### It flagged something harmless, or missed a secret. What now?

Please tell us with the "False positive or false negative" issue template, using a **masked** or look-alike snippet,
never the real value. Reports like these are how the next versions improve.

### Is it fast enough for CI?

For changed files, yes: a typical commit is a few kilobytes, which takes well under a second. Scanning a large
repository from scratch takes much longer, because a neural network reads every byte (about 130 KB/s on a 12-core
desktop); do that once and then scan changes. Figures in [performance.md](performance.md).

### Can it scan only what changed?

Yes. The CLI can scan the files of a git diff or the staged changes and keep only the findings on added lines, which
is what the [pre-commit hook](../integrations/pre-commit/) and the [GitHub Action](../integrations/github-action/) use.
See [cli.md](cli.md#scan).

### What can `secrets-bin` read?

Executables and libraries (PE, ELF, Mach-O), object files and `ar` archives, Java class files, Python bytecode,
WebAssembly and firmware images, and the zip, JAR, APK, wheel, gzip and tar containers around them, opened with limits
on depth, size and compression ratio. By default a finding must touch a printable string (16 or more ASCII or UTF-16
characters); `--all-bytes` reports the others too.

## The personal-data example

### What does `pii` mask?

Every byte that its training data label as personal data, as one type, `PII`: names, e-mail addresses, phone numbers,
postal addresses, identity, account, card and IBAN numbers, IP addresses, dates, user names, URLs and more. That is the
policy of the PII Masking Benchmark: dates and identifiers count as personal data. Use `--bias` to trade recall for
precision.

### Which languages does `pii` read?

It learned from documents in 23 languages (Bulgarian, Croatian, Czech, Danish, Dutch, English, Estonian, Finnish,
French, German, Greek, Hungarian, Italian, Latvian, Lithuanian, Polish, Portuguese, Romanian, Serbian, Slovak,
Slovenian, Spanish and Swedish); bytes are bytes, so it reads any encoding, but quality is measured on those. On the
benchmark's multilingual task it scores F2 0.950.

### Does `pii` make my data compliant?

No model does by itself. `pii` reduces exposure: it masks what it finds, and it misses some of it (on the benchmark's
hardest tasks, protocol traces and EU legal texts, it scores F2 0.54 and 0.33). Use it as one control among others, and
measure it on your own data.

### Can I mask personal data before it reaches an LLM?

Yes: run `purebyte serve --model pii` next to your application and send each prompt through `POST /v1/redact` before
it leaves your network. The markers are consistent (the same value is `[PII_1]` everywhere in a document), so the
redacted prompt still reads.

## The model

### Why ternary weights?

They cost accuracy and save space. They keep the files small: 2.25 bits per weight, scales included, against 16 or 32
bits for the usual formats, and the models are trained with them from the first step (quantization-aware training), so
there is no conversion afterwards. In our experiments on clean binaries, a float control made 2.4 times fewer false
alarms than the same ternary model, while adding the n-gram memory cut the ternary model's 13 times; a float model
with the memory was not trained, so ternary plus memory is what we ship, not a proven optimum
([design experiments](../benchmarks/RESULTS.md#design-experiments)). The three example specialists are ternary.

### Why state-space blocks and not attention?

A state-space block reads a sequence in one linear pass with a fixed-size state, so the cost grows linearly with the
input and a long input can be streamed. At the byte level, where sequences are several times longer than in tokens,
that matters. The runtime also has an attention block for models that need one.

### What is the n-gram memory?

Large tables of learned vectors, indexed by a hash of the last 2, 3 and 4 bytes at each position, whose rows are added
to the byte embedding. They let a small network recognize many short patterns (key prefixes, code idioms, formats)
with three table reads per byte. They are stored at 4 bits per value, and they hold 6.3 M of `pii`'s 8.3 M parameters
and 50 M of `secrets-bin`'s 54 M.

### Can it handle long files and long contexts?

Long files, yes: they are read in overlapping windows (512 bytes, one every 384), so length only costs time, linearly.
Long context, not yet: each decision of the released examples sees 512 bytes. The model format has a stream context,
in which a causal model reads an input of any length in chunks and carries its state from chunk to chunk, with the
same hidden states as a single pass, but no released model or recipe uses it yet ([roadmap](roadmap.md)). Training
longer windows costs linearly more compute and memory (a `nano` model with 1,024-byte windows and batches of 32 trains
in 5.9 GB of GPU memory).

### Will I get the same results on another machine?

On the same platform, bit for bit, with any kernel and any number of threads. Across operating systems, raw scores can
differ slightly, because math libraries differ (the tests allow a relative difference of 1e-4; observed differences
use about 1.5 % of that), and decisions and spans are expected to agree, which the golden tests check
([architecture.md](architecture.md#numerics-and-exactness)).

### What does "confidence" mean?

The model's probability for a span or a decision. It is useful for ranking findings from the same model, but it is not
calibrated yet, so a 0.9 does not mean "right nine times out of ten". Calibration is on the roadmap.

### What are "votes"?

`secrets-code` is one model by default, so `votes` is 1. The optional `secrets-code-ensemble` runs it together with two
more models trained the same way from other random seeds, and a finding needs two of the three to agree; `votes` says
how many agreed. On CredData the ensemble finds slightly more (F1 0.805 against 0.797) for three times the work.

### What does `--bias` do?

It moves the operating point: a positive bias makes the model mark more (higher recall), a negative one less (higher
precision). Each file stores the operating point chosen on validation data. For `secrets-code`, `--bias -2` gives
precision 0.932 and recall 0.663 on CredData TEST, against 0.879 and 0.729 by default
([benchmarks/RESULTS.md](../benchmarks/RESULTS.md#operating-points)).

## Training

### Can I train my own specialist?

Yes. The training stack, the recipes of the three example specialists and the scripts that build their data from public
sources are in their own repository, [purebyte-train](https://github.com/purebyte-ai/purebyte-train). Describe the
specialist you need to an AI coding agent (Vibe Training), or follow the steps yourself:
[training.md](https://github.com/purebyte-ai/purebyte-train/blob/main/docs/training.md). The training stack trains on a
CPU, and on AMD Radeon GPUs (tested on Windows); NVIDIA, Linux ROCm and Apple silicon GPUs use the same stock PyTorch
code but have not been run by the maintainers yet. Today it builds span-finding specialists, like the three examples;
decisions, labels, scores and maps come next ([roadmap](roadmap.md)). Any model exported in the open format
([spec/FORMAT.md](../spec/FORMAT.md)) runs on this runtime ([customize.md](customize.md)).

### What is Vibe Training?

Training an AI Specialist by describing what you want to an AI coding agent, the way vibe coding builds software. The
agent follows the golden path in [the AGENTS.md of purebyte-train](https://github.com/purebyte-ai/purebyte-train/blob/main/AGENTS.md#the-golden-path-vibe-training): it chooses or creates the specialist's recipe,
downloads or generates the data, writes the success criteria before measuring anything, trains, evaluates honestly and
publishes. The training stack refuses the usual mistakes, such as test data leaking into training or a criterion
changed after the fact.

### What does training cost?

Little. The three example specialists were trained from scratch, with no pretrained body, on one consumer GPU (an AMD
Radeon RX 6800 XT with 16 GB): the `secrets-code` recipe trains its three seeds and its control in 25 minutes, and the
three specialists took about 2.5 GPU-hours in all, with their bias sweeps and checks
([conditions](performance.md#training-time)). Each training run reads 71 to 123 million bytes of windows, and a smoke
recipe proves the whole pipeline on a CPU in minutes. Most of the work of a new specialist is its data: a generator,
look-alikes, and real data kept apart for the exams.

### Which GPU do I need to train?

The maintainers train on an AMD Radeon RX 6800 XT (16 GB) under Windows, with PyTorch's ROCm build and
`--device cuda`; `--backend radeon` adds fused kernels there that make a step more than twice as fast. NVIDIA (CUDA,
`--device cuda`), AMD on Linux (ROCm) and Apple silicon (`--device mps`) use the same stock PyTorch code but have not
been run by the maintainers yet; `--device auto` takes the best device it finds. A `nano` model with 512-byte windows
and batches of 16 takes 1.8 GB, so four train side by side on a 16 GB card (the released recipes step with 20 and 23
windows). No GPU is needed: the CPU is the default. On a CPU, the trainer computes bit for bit what the code that
trained the released examples computes on a CPU; the released weights were trained on a GPU and are reproduced
closely, not bit for bit ([below](#can-i-reproduce-the-released-models)). What has been tested on which hardware, and
the PyTorch build for yours:
[Training on any hardware](https://github.com/purebyte-ai/purebyte-train/blob/main/training/docs/gpu.md), in
purebyte-train.

### Can I reproduce the released models?

The recipes, the generators and the data scripts are in [purebyte-train](https://github.com/purebyte-ai/purebyte-train),
and a retraining lands close to the released models. Not bit for bit: the released weights were trained on a GPU
(whose kernels round differently from the CPU reference), and part of their data (earlier code corpora, and binaries
that cannot be redistributed) differs from what the scripts rebuild today. Each specialist's README there says exactly
what rebuilds and what does not.

## Licensing

### Is PureByte open source?

The code is: the engine, the CLI, the HTTP server, the Python bindings, the model format specification, the reference
implementation and the integrations are Apache-2.0, and so are the training stack, the specialists' recipes and the
data scripts in [purebyte-train](https://github.com/purebyte-ai/purebyte-train). The weights of the example
specialists are distributed under a separate license, see [MODEL_LICENSE.md](../MODEL_LICENSE.md).

### Can I use it commercially?

Yes. The code is Apache-2.0. The released weights are free to use for any purpose, commercial use included, on
hardware you own or control, including cloud instances and CI runners that run on your behalf, under the
[PureByte Model License](../MODEL_LICENSE.md): what it does not allow is redistributing the weights, building models
from them, or offering them to others as a hosted service whose main value is the model itself. The outputs are yours,
and so is any model you train yourself with the training code.

### Can you build a specialist for my data?

Yes: write to [pablo@purebyte.ai](mailto:pablo@purebyte.ai). The same address handles other license terms.
