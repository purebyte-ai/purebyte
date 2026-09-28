# Reproduce the PII Masking Benchmark results

This folder scores a PureByte model on the [PII Masking Benchmark](https://huggingface.co/datasets/piimb/pii-masking-benchmark)
(PIIMB) with the metric of its [leaderboard](https://huggingface.co/datasets/piimb/pii-masking-benchmark-results), from
public material only: the benchmark, the released `purebyte` CLI and the released `pii` model. The results and their
conditions are in [RESULTS.md](../RESULTS.md#pii-on-the-pii-masking-benchmark).

| File | What it does |
|---|---|
| [`score.py`](score.py) | Writes the benchmark's sentences as files, scores the CLI's reports (or character spans from any tool) with the leaderboard's metric, and gathers several models' scores into exam metrics. Python 3.8+, standard library only |
| [`run.sh`](run.sh) | The whole procedure for one model: model download, inputs, one scan, scores |

**The benchmark's license is CC BY-NC 4.0.** Use it to measure, locally. Never train on it, never redistribute it,
and publish scores only. Nothing in this folder prints a text of the benchmark.

## 1. Get PIIMB

Either with the data scripts, which pin the revision and check the files' sha256, in a checkout of
[purebyte-train](https://github.com/purebyte-ai/purebyte-train):

```bash
python data/fetch_exams.py --set piimb        # -> $PUREBYTE_DATA/exams/piimb/test.jsonl, test_sentences.jsonl
```

or by hand, the same revision (`7d797b9f`):

```bash
mkdir piimb && cd piimb
curl -LO https://huggingface.co/datasets/piimb/pii-masking-benchmark/resolve/7d797b9fc8dc1942cef60fbe532e7d1a0e31b655/data/test_sentences.jsonl
```

`score.py` warns when the file is not that copy (sha256 `5ff46f3a80316318...`): other revisions give figures that are
not comparable.

## 2. Install PureByte and run

Install the CLI ([docs/install.md](../../docs/install.md)), then from any working directory:

```bash
bash /path/to/purebyte/benchmarks/piimb/run.sh /path/to/piimb pii
```

The script pulls `pii`, writes the 150,022 sentences as files under `./piimb-results/inputs/`, scans them with one
`purebyte scan` (every sentence on its own, as every model of the leaderboard runs), and prints the table of
[RESULTS.md](../RESULTS.md#pii-on-the-pii-masking-benchmark) into `./piimb-results/pii/scores.md` (all figures in
`scores.json`). A model file works too: `run.sh /path/to/piimb model.gguf`.

`BIAS=2 run.sh ...` scores another operating point (`--bias`), `THREADS=8` sets the CLI's threads, and
`EXCLUDE=PIIMB_OVERLAP.json` also scores without the sentences listed there (next section).

## The metric

PIIMB scores **masking**, not entity recognition: which characters a model would hide.

- **Label-agnostic, per character** (Python code points). A character is predicted when any predicted span covers it,
  and gold when any entity of the sentence covers it; overlapping or touching spans are merged on each side first.
- **Micro-averaged per task.** Predicted, gold and shared characters are summed over a task's sentences; then
  precision = shared / predicted, recall = shared / gold, F1, **F2** (β = 2: recall weighted above precision) and FPR =
  (predicted − shared) / (characters − gold).
- **The leaderboard ranks by the mean F2 of the six tasks**: `ai4privacy-en`, `ai4privacy-multi` (23 languages),
  `nemotron-pii`, `gretel`, `privy` (protocol traces: JSON, XML, SQL), `mapa-eur-lex` (legal texts in 21 languages).
  Precision and recall are averaged the same way.
- Every model runs on the **`sentences`** subset: the documents cut into sentences, each scanned on its own.
- The CLI reports byte offsets in the UTF-8 input; a span becomes characters rounding outwards (a boundary inside a
  multi-byte character takes the whole character).

`score.py` is a replica of the benchmark's `compute_masking_metrics`. Two reference points it prints with
`score.py trivial --piimb DIR`: masking **every** character scores a mean F2 of 0.4423 at a mean precision of 0.157
(F2 rewards over-masking, which is why the `pii` recipe also requires a mean precision of at least 0.70), and masking
nothing scores 0.

## Overlap with training data

The `pii` model learned from TRAIN documents of three of the datasets the benchmark samples its tests from
(OpenPII-1M, Nemotron-PII and Gretel), never from a test or validation split. The training pool was built with a hard
check that drops any document sharing a sentence or a masked template with a benchmark document
([data/README.md](https://github.com/purebyte-ai/purebyte-train/blob/main/data/README.md#the-pii-pool) in
purebyte-train). Documents of one synthetic generator still share phrases across its splits; in a purebyte-train
checkout, `python data/build_pii_pool.py --piimb-overlap` lists the benchmark sentences that share at least one
64-byte block with the training pool (`PIIMB_OVERLAP.json`, kept locally: it is derived from the benchmark), and
`score.py score --exclude PIIMB_OVERLAP.json` (or `EXCLUDE=...` with `run.sh`) scores the benchmark without them.
[RESULTS.md](../RESULTS.md#pii-on-the-pii-masking-benchmark) gives both figures.

## Score other tools

`score.py score` takes several reports at once:

```bash
python3 score.py score --piimb piimb \
  --report pii=piimb-results/pii/purebyte.jsonl \
  --spans other-tool=other-tool-spans.jsonl
```

`--spans` takes JSON Lines `{"uid": "<the sentence's uid>", "spans": [[start, end], ...]}` with character offsets, so
any masking model can be scored with a few lines of glue code, in the same harness. The benchmark repeats some uids:
write one line per sentence, in the benchmark's order, and the k-th line of a uid goes to its k-th sentence.

## The recipe's criteria (six seeds)

The `pii` recipe
([specialists/pii/recipes/pii-1.0.0.yaml](https://github.com/purebyte-ai/purebyte-train/blob/main/specialists/pii/recipes/pii-1.0.0.yaml)
in purebyte-train) judges the mean of its six seeds. After a training run, export every seed and score each file, then
record the metrics. These commands run in a checkout of [purebyte-train](https://github.com/purebyte-ai/purebyte-train),
whose `benchmarks/piimb/score.py` is a pinned copy of the one here; `run.sh` comes from this repository:

```bash
python specialists/pii/eval/export_seeds.py specialists/pii/recipes/pii-1.0.0.yaml --out seeds/
for f in seeds/seed_*.gguf; do bash /path/to/purebyte/benchmarks/piimb/run.sh /path/to/piimb "$f"; done
python3 benchmarks/piimb/score.py metrics --released piimb-results/seed_<released>/scores.json \
    --seeds piimb-results/seed_*/scores.json --out piimb-metrics.json
python -m pbtrain eval specialists/pii/recipes/pii-1.0.0.yaml --record piimb --metrics piimb-metrics.json
```

The "PIIMB mode" operating point (per seed, the bias and window gate that maximize the byte F2 on the run's own
`select` documents cut into sentences) is reported beside the default one, also from a purebyte-train checkout:
`python specialists/pii/eval/piimb_mode.py specialists/pii/recipes/pii-1.0.0.yaml --piimb /path/to/piimb`. It runs the
checkpoints in PyTorch with the engine's windows, because the CLI cannot switch the window gate off.
