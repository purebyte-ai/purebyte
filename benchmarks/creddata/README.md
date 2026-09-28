# Reproduce the CredData results

This folder rebuilds the [secrets-code results](../RESULTS.md#secrets-code-on-creddata) from public material only: the
official CredData download, the released `purebyte` CLI and the released `secrets-code` model. Both published TEST
tables come out of one scan: the one without the four repositories that overlap the training corpus (the headline
figures) and the complete partition.

| File | What it does |
|---|---|
| [`score.py`](score.py) | Builds the split, lists the files to scan, writes exact scan regions and scores the reports of PureByte, gitleaks, trufflehog or any tool. Python 3.8+, standard library only |
| [`run.sh`](run.sh) | Runs the whole procedure for one partition: model download, scan, optional gitleaks and trufflehog, scores |

Nothing in this folder prints, stores or sends a credential value, and trufflehog always runs with
`--no-verification`.

## 1. Get CredData

The official script needs a Linux-like system (some original file names are not valid on Windows; the finished
dataset is fine on any system) and plenty of temporary disk space, because it fetches a full snapshot of each of the
337 repositories. The final dataset is about 1 GB.

```bash
git clone https://github.com/Samsung/CredData.git
cd CredData
git checkout c09c0c52fc6dae4ae5438ae69ba486f9f8059f0d   # the snapshot we measured (6 September 2026)
python3 -m pip install -r requirements.txt
python3 download_data.py --data_dir data --jobs 8        # downloads, extracts and obfuscates
```

A few dataset files are the source code of security tools and may trigger antivirus alerts. They are only read,
never executed.

Check that you have the same dataset and split as we do:

```bash
python3 /path/to/purebyte/benchmarks/creddata/score.py split --creddata .
```

```text
CredData: 67564 meta rows, 64474 labelled units, 337 repositories
test             168 repositories ·  5325 labelled files ( 852.2 MiB) · up to 4 MB:  5262 files ( 356.8 MiB), 5295 positive units
dev              169 repositories ·  5704 labelled files ( 117.2 MiB) · up to 4 MB:  5701 files (  95.7 MiB), 9640 positive units
dev-no-openssl   168 repositories ·  5461 labelled files ( 109.5 MiB) · up to 4 MB:  5458 files (  88.0 MiB), 4485 positive units
all              337 repositories · 11029 labelled files ( 969.3 MiB) · up to 4 MB: 10963 files ( 452.5 MiB), 14935 positive units
```

(Our copy lacks one labeled file that an antivirus removed as a false alarm; with the complete dataset the two DEV
rows and `all` list one more file.)

The published TEST figures leave out four repositories that overlap the training corpus
([why](../RESULTS.md#training-data-overlap)). `--exclude-repo training-overlap`, an option of every `score.py`
command, does the same:

```bash
python3 /path/to/purebyte/benchmarks/creddata/score.py split --creddata . --partitions test --exclude-repo training-overlap
```

```text
CredData: 67564 meta rows, 64474 labelled units, 337 repositories; left out: c8aa9b49, e13d78bf, f710ac3c, fc8343f4
test without c8aa9b49, e13d78bf, f710ac3c, fc8343f4  164 repositories ·  4800 labelled files ( 843.4 MiB) · up to 4 MB:  4737 files ( 348.0 MiB), 4219 positive units
```

The option takes CredData repository ids too (`--exclude-repo f710ac3c,fc8343f4`), and it never changes the split:
the repositories are left out after it is computed.

## 2. Install PureByte and run

Install the CLI (see [docs/install.md](../../docs/install.md)), then from any working directory:

```bash
bash /path/to/purebyte/benchmarks/creddata/run.sh /path/to/CredData test
```

The script pulls `secrets-code`, writes the scan regions, scans them, runs gitleaks and trufflehog if they are on
`PATH`, and prints the tables of [RESULTS.md](../RESULTS.md) twice (run it again with `MODEL=secrets-code-ensemble`
for the rows of the optional ensemble): for every repository of the partition
(`scores.md`, `scores.json`) and without the four overlapping repositories (`scores-excluded.md`,
`scores-excluded.json`; set `EXCLUDE_REPOS` to other ids, or to nothing to skip it). Everything lands in
`./creddata-results/test/`.

| Mode | Bytes to scan on TEST | Time on a desktop CPU |
|---|---:|---|
| Scan regions (default) | 25.2 MiB | about 3 minutes with the model, three times that with the ensemble |
| `--whole-files` | 356.8 MiB | about 45 minutes with the model, three times that with the ensemble |

The times are estimates from the throughput measured on an idle 12-core desktop (about 130 KB/s with the default
threads, [docs/performance.md](../../docs/performance.md)); both modes give the same scores.

## The split

`score.py` computes the split from the official meta files, so there is no split file to download. Repositories
are sorted by their number of positive units (ties broken by a seeded hash), taken in consecutive pairs, and a
coin seeded with `20260922` sends one member of each pair to DEV and the other to TEST. Both halves have the same
number of repositories and a similar number of positives. `score.py split --json split.json` writes the assignment
of every repository.

## The metric

CredData labels candidate lines as credentials (`T`) or not (`F`, `X`). A unit is one labeled line range, the
official key `(FilePath, LineStart, LineEnd)`; it is positive if any of its rows is `T`.

- A positive unit is found when any line flagged by the tool falls inside it.
- A negative unit is a false positive when a flagged line falls inside it and inside no positive unit.
- Each unit counts once. Flagged lines outside every labeled unit do not count, as in the official benchmark.
- A finding that spans several lines (a PEM block, for example) flags all of them.
- The 95 % interval of F1 comes from 1,000 bootstrap resamples of repositories (those that hold a positive unit or a
  false positive of the tool). Precision, recall, F1 and the interval are rounded only when printed.
- `score.py score --paired A,B` also prints F1(B) − F1(A) with a paired interval: the same resamples score both
  reports, so the interval is that of the difference. `--drop-lines NAME=TSV` scores a report without the labeled
  units that hold a listed line; give the same report twice, under two names, to measure what those lines change.

## Scan regions

The scanner reads a file in independent windows of 512 bytes that start every 384 bytes. A scan region starts at a
multiple of 384 bytes and ends at a window end, with 768 bytes of context on each side of every labeled line, so
the scanner sees exactly the windows it would see in the whole file around each label. Findings on labeled lines
are therefore identical; we checked this on a sample, where whole files and regions gave the same units and the same
flagged lines. Regions only change what happens far from any label, which the metric does not count.

`score.py regions` writes the regions and a `regions.json` map; `score.py score --regions DIR` maps every finding back
to its file and line.

## Score other tools

`score.py score` accepts several reports at once, each with an optional display name:

```bash
python3 score.py score --creddata CredData \
  --purebyte secrets-code=creddata-results/test/purebyte.json --regions creddata-results/test/regions \
  --gitleaks gitleaks=gitleaks.json \
  --trufflehog trufflehog=trufflehog.jsonl \
  --flags mytool=mytool.jsonl \
  --partitions test,dev-no-openssl --by-category --exclude-repo training-overlap
```

`--flags` takes JSON Lines of the form `{"f": "data/<repo>/<scope>/<file>", "flags": [{"l": 12, "n": 1}]}` (first
flagged line and number of lines), so any scanner can be scored with a few lines of glue code.
