# Performance

Measured on an idle AMD Ryzen 9 5900X (12 cores, 24 threads, AVX2, no AVX-512; 1.8 % CPU load before the session),
Windows 10, with the release build of the runtime (MSVC, static runtime) and the released model files, one request or
one scan at a time, model already loaded unless said otherwise. Other machines will differ; the last section says how
to measure yours.

## Small decisions

One window of each specialist, run by the engine with its threads working on that single window. Medians of 40
runs. The 1,024-byte rows are one window of 1,024 bytes, twice the examples' 512-byte window; in normal use a
1,024-byte input runs as three windows of 512 bytes, one every 384 bytes (as in the `purebyte bench` figures below):

| Specialist | Window | 1 thread | 8 threads | Speed-up |
|---|---|---:|---:|---:|
| `secrets-code` | 64 bytes | 2.97 ms | **0.82 ms** | 3.6 |
| `secrets-code` | 256 bytes | 12.0 ms | **2.81 ms** | 4.3 |
| `secrets-code` | 1,024 bytes | 50.4 ms | **10.5 ms** | 4.8 |
| `pii` | 64 bytes | 3.20 ms | **0.89 ms** | 3.6 |
| `pii` | 256 bytes | 13.1 ms | **3.10 ms** | 4.2 |
| `pii` | 1,024 bytes | 54.8 ms | **13.7 ms** | 4.0 |
| `secrets-bin` | 64 bytes | 5.92 ms | **1.45 ms** | 4.1 |
| `secrets-bin` | 256 bytes | 24.3 ms | **4.95 ms** | 4.9 |
| `secrets-bin` | 1,024 bytes | 102 ms | **24.5 ms** | 4.2 |

- One window gets no faster beyond about 8 threads (12 threads: 0.92 ms at 64 bytes, 11.4 ms at 1,024 bytes for
  `secrets-code`), so when a batch has fewer windows than threads, the idle threads join its windows up to 8 per
  window (`--intra-threads`, [cli.md](cli.md#choosing-a-model)).
- Every thread count gives bit-identical results (the same digests of the hidden states and head values).
- `secrets-bin` has twice the blocks of the others (8 against 4) and a larger n-gram memory, hence about twice the
  time.
- The optional `secrets-code-ensemble` runs three models, so a decision costs about three single models.
- Many small decisions at once: a batch of 48 windows of 64 bytes, on 12 threads with the default automatic
  `--intra-threads`, runs at about 2,700 windows per second with `secrets-code`, 2,600 with `pii` and 1,300 with
  `secrets-bin` (the median of 4 runs of the batch test of `window_latency`, [below](#measure-it-yourself)).

Through the CLI, `purebyte bench` measures the model's path for a small input (windows, model, span decoding; not
the profile's post-processing): with the default threads, 0.92 ms for 64 bytes and 10.3 ms for 1,024 bytes with `secrets-code`,
1.21 ms and 10.9 ms with `pii`, and 2.01 ms and 20.5 ms with `secrets-bin` (medians of 40 runs, `--repeats 40`).

## Whole inputs

Scans with the CLI's default options and threads (23), wall-clock time of the whole command, the median of three runs:

| Specialist | Input | Time | Throughput | Peak memory |
|---|---|---:|---:|---:|
| `secrets-code` | PureByte's own source code (engine, CLI, training stack and specialists, before they were split into two repositories): 2.1 MB read | 16.3 s | 131 KB/s | 187 MB |
| `pii` (`redact`) | 3.9 MB of the PII Masking Benchmark's sentences, as JSON Lines | 30.9 s | 126 KB/s | 239 MB |
| `secrets-bin` | one Windows DLL of 5.0 MB | 77 s | 65 KB/s | 219 MB |

**What this means in practice.** A commit touches a few kilobytes: well under a second, so pre-commit hooks and CI
checks on changed files are comfortable. A 1 MB file takes about 8 seconds with `secrets-code` or `pii`. A full scan
of a large repository takes minutes to hours at these rates: do it once, then scan changes (`--staged`,
`--git-diff`).

Pattern-based scanners are much faster per byte. In an earlier session on this machine, while it was busy with other
work (6 threads; so this is an approximate ratio, not a measurement by our idle-machine rule), gitleaks read the 969 MiB
of CredData roughly 80 times faster than `secrets-code` reads code above, and trufflehog (without verification)
roughly 25 times faster. PureByte reads every byte with a neural network; the point is what it finds, not raw
throughput.

## Compared with Laya and Jev

[Laya](https://huggingface.co/convaiinnovations/laya) is a non-generative decision model (ModernBERT-large and a
decision head, 421 M parameters). We ran it on the same idle machine as the tables above: the `laya` 0.3.6 package,
its English checkpoint (808 MB), PyTorch 2.14 on the CPU, model already loaded, one three-option question ("is this
value a real secret, a placeholder, or not a secret?") about one line of code. Medians of 15 calls after 3 warm-up
calls, beside `secrets-code` on one window of 256 bytes, which holds that line with room to spare:

| Threads | Laya, the line alone (173 bytes; 190 tokens with the question) | Laya, the line and two lines around it (327 bytes; 244 tokens) | PureByte `secrets-code`, one 256-byte window |
|---:|---:|---:|---:|
| 1 | 1,382 ms | 1,756 ms | 12.0 ms |
| 4 | 501 ms | 686 ms | 3.54 ms |
| 8 | 421 ms | 451 ms | **2.81 ms** |
| 12 | **373 ms** | 448 ms | 3.13 ms |
| 24 | 395 ms | 462 ms | |

- At each one's best thread count, one decision takes 373 ms with Laya and 2.81 ms with PureByte, 133 times less;
  on one thread, 115 times less. Loading Laya's checkpoint took 21.7 s; starting the `purebyte` CLI, loading the
  model and answering takes about 0.05 s on the same Windows machine ([Loading](#loading) gives the figures under
  Linux).
- Figures published by others, not measured here: [Laya's model card](https://huggingface.co/convaiinnovations/laya)
  gives 39.5 ms for one question with the English checkpoint (32.8 ms with the multilingual one) on an NVIDIA T4 GPU, on
  the questions of its own benchmark, and 193 to 464 ms on a CPU; it cites third-party measurements of TypeSafe Jev's
  hosted API at 236 to 276 ms (median): [AbdelStark/jev-benchmarks](https://github.com/AbdelStark/jev-benchmarks) and
  [nibzard/decision-model-benchmark](https://github.com/nibzard/decision-model-benchmark). Retrieved on 2026-09-22.
- What this compares is the cost of one small decision, and the two sides differ in more than their architecture: a
  1.9 M-parameter specialist, run by the engine alone (no CLI, no post-processing), against a 421 M-parameter general
  model called through its Python package, with the question and its options as part of its input. Most of the ratio
  comes from that difference in size and generality. It does not compare quality, and the jobs differ: Laya and Jev
  answer a question written at request time; a PureByte specialist answers the one question it was trained for.

## Compared with a general language model

Indicative only: the machine was busy with other work. On the same desktop CPU, a 4-billion-parameter language model
(Qwen3-4B-Instruct-2507, 4-bit `Q4_K_M` weights, llama.cpp on the CPU with 12 threads and four parallel slots) labeled
15 to 19 candidate credentials a minute, each one a few lines of code after a fixed prompt of about 1,300 tokens that
the server keeps in its cache, with a JSON answer of 8 tokens (22-23 September 2026). That is seconds per decision,
against a few milliseconds for a specialist on a small input ([above](#small-decisions)). A general model answers open
questions that a specialist cannot; this compares only the cost of one narrow decision.

## Loading

Starting the CLI, loading the model and answering one small decision (`purebyte decide` on a 29-byte input), end to
end, on the same desktop under Linux x86-64 (WSL2): 0.01 s and 9.4 MB of peak memory for `secrets-code`, 0.03 s and
13 MB for `pii`, and 0.16 s and 39 MB for `secrets-bin`, with 1 or 23 threads alike. Peak memory is the maximum
resident set size of the whole process, measured with `/usr/bin/time` ([Memory](#memory)).

## Measured speed-ups

Ratios measured on the same machine in the same session, each against the engine without the change:

| Change | Where | Speed-up | Output |
|---|---|---:|---|
| Current kernel pack (default) | Per window, secrets-code-sized model / secrets-bin-sized model | 1.33-1.39 / 1.29-1.32 | Bit-identical |
| Current kernel pack (default) | End to end, scanning source code | 1.26 | Identical findings |
| String prefilter (opt-in) | End to end, real binaries (it skips 81 to 89 % of their windows) | 5.2 | Loses only findings outside printable strings |
| Kernel pack and early exit (models with an exit head) | End to end, real binaries, prefilter off, against the previous engine | 2.05 | Same number of findings in that test |
| Kernel pack, prefilter and early exit together | End to end, real binaries, against the previous engine with none of them | 13.2 | Loses only findings outside printable strings (the prefilter's effect) |
| Threads sharing one window (default) | One 64-byte window, 8 threads against 1 | 3.6 | Bit-identical |

None of the released examples ships an exit head, so the two rows with early exit do not apply to them: they were
measured with a research model that had one.

## Where the time goes

This profile predates the current kernel pack. At the time, the ternary matrix products took about two thirds of the
time per window and the state-space scan most of the rest; heads, decoding and the n-gram memory took a few percent
(the heads, 3 % of a one-thread window in the current build). The CPU time per window was the same with 1 and 12
threads, so the engine was not limited by memory bandwidth.

## Training time

Wall-clock time of the training stage of each released recipe, from the logs of the release runs, on one AMD Radeon
RX 6800 XT (16 GB) under Windows 10 with PyTorch's ROCm build:

| Recipe | Runs | At a time | Training | Date |
|---|---|---:|---:|---|
| `secrets-code` 1.0.0 (`nano`, 12,000 steps of 20 windows) | 3 seeds and the control | 4 | 1,468 s (24.5 min) | 23 September 2026 |
| `pii` 1.0.0 (`nano_n15`, 12,000 steps of 16 windows) | 6 seeds and 2 controls | 1 | 432 to 461 s per seed, about 60 min for the eight runs | 24 September 2026 |
| `secrets-bin` 1.0.0 (`tiny_n18`, 6,000 steps of 23 windows) | 6 seeds and the control | 2 | 3,459 s (58 min) | 24 September 2026 |

`secrets-code` and `secrets-bin` ran with the fused kernels of the training stack's Radeon backend
(`--backend radeon`). With their bias sweeps and checks, the fine-tuning stages took 1,590 s, 3,665 s and, for the two
runs of three `pii` seeds, 1,802 s and 1,783 s: about 2.5 GPU-hours for the three examples. The data stages took 18 to
94 seconds each, and the exams on real data run on the CPU. On your hardware, `python -m pbtrain run RECIPE --device
cuda --parallel N` in [purebyte-train](https://github.com/purebyte-ai/purebyte-train) prints the time of every stage.

## Measure it yourself

```bash
purebyte bench --model secrets-code
```

`purebyte bench` reports, on your machine, the latency of the main model on inputs of several sizes (median and 95th
percentile) and its throughput on a longer input ([cli.md](cli.md#bench)). The thread-scaling table and the batch
figures above come from a tool built with the test suite (`-DPUREBYTE_BUILD_TESTS=ON`, the default), in the build's
`tests` folder; its batch test takes 48 windows unless `--batch` says otherwise:

```bash
window_latency path/to/model.gguf --size=64 --iterations=40 --threads=1,2,4,8,12
```

`purebyte info secrets-code` prints the path of an installed model file. Numbers from other machines are welcome in an
issue: include the CPU, the operating system, the thread count and whether the machine was idle.

## Memory

Peak memory of the whole process: the maximum resident set size, measured with `/usr/bin/time` on the same desktop
under Linux x86-64 (WSL2).

| Specialist | One small decision (`purebyte decide`, 29 bytes, 1 or 23 threads) | A larger input (23 threads) |
|---|---:|---|
| `secrets-code` | 9.4 MB | 188 MB over 149 source files (1.8 MB), in 15 s; with 4 threads, 42 MB in 35 s |
| `pii` | 13 MB | 182 MB to redact 133 KB of Markdown |
| `secrets-bin` | 39 MB | 220 MB on a Windows DLL of 4.8 MB, in 82 s |

The engine holds the whole model file in memory, plus one byte per ternary weight (the 2-bit codes are unpacked once,
at load); the 4-bit n-gram tables stay packed and are decoded row by row as they are read. While scanning, each thread
keeps scratch buffers for the windows it works on, so fewer threads use less. The Windows figures of
[Whole inputs](#whole-inputs) (187, 239 and 219 MB) agree. Model files are small: 0.98 to 28.8 MiB.
