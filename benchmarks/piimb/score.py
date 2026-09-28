#!/usr/bin/env python3
"""Score PII masking on the PII Masking Benchmark (PIIMB) with the leaderboard's metric.

Standard library only (Python 3.8+). PIIMB is licensed CC BY-NC 4.0: download it yourself, use it to MEASURE only, never
train on it and never redistribute it. Nothing here prints a text or a value of the benchmark: the scorer reads
offsets, and the benchmark's texts only to cut and count characters.

Subcommands:

    inputs   write every sentence of the `sentences` subset as one file, for the purebyte CLI to scan
    score    score one or more reports (the CLI's `scan --format jsonl`, or character spans from any tool)
    trivial  the reference points: mask every character, mask nothing
    metrics  gather the scores of several models (the seeds of a recipe) into the metrics file of `pbtrain eval`

Examples:

    python3 score.py inputs  --piimb PIIMB --out work/
    purebyte scan --model pii --format jsonl work/inputs > work/purebyte.jsonl
    python3 score.py score   --piimb PIIMB --inputs work/ --report pii=work/purebyte.jsonl --json work/scores.json
    python3 score.py score   --piimb PIIMB --spans other-tool=spans.jsonl
    python3 score.py metrics --released scores-released.json --seeds scores-seed*.json --out metrics.json

The metric (a replica of PIIMB's `compute_masking_metrics`, the leaderboard's ranking):

* label-agnostic and per CHARACTER (Python code points): a character is predicted when any predicted span covers it,
  and gold when any entity of the sentence covers it; overlapping or touching spans are merged on each side first;
* per task, micro-averaged: predicted, gold and shared characters are summed over the task's sentences, then
  precision = shared / predicted, recall = shared / gold, F1, F2 (recall weighs four times precision) and FPR =
  (predicted - shared) / (characters - gold);
* the leaderboard's number is the MEAN of the six tasks' F2 (ai4privacy-en, ai4privacy-multi, nemotron-pii, gretel,
  privy, mapa-eur-lex); precision and recall are averaged the same way.
Every model runs on the `sentences` subset: each sentence is scanned on its own. Byte spans (the CLI reports offsets in
the UTF-8 bytes of the input) become character spans rounding outwards: a boundary inside a multi-byte character takes
the whole character.

`--exclude FILE` (a JSON object with a `uids` list, such as the PIIMB_OVERLAP.json that data/build_pii_pool.py writes)
scores a second time without those sentences: the figures without the part of the benchmark that shares content with
a training set.
"""
import argparse
import hashlib
import json
import os
import sys

TASKS = ("ai4privacy-en", "ai4privacy-multi", "nemotron-pii", "gretel", "privy", "mapa-eur-lex")
FILES = {"sentences": "test_sentences.jsonl", "full_text": "test.jsonl"}
# the copy of PIIMB these figures were computed on (data/exams/pii.yaml pins the same revision)
REVISION = "7d797b9fc8dc1942cef60fbe532e7d1a0e31b655"
SHA256 = {"test_sentences.jsonl": "5ff46f3a80316318794f94596fa374060d70f2f32a85909f958cbabc70bae41f",
          "test.jsonl": "400c7fffcc57f7b5a7c19db600eae0fc9205539daf320a7cd25e6eb56fcaca4c"}
SHARD = 1000


# ---------------------------------------------------------------------------------------------------- the benchmark
def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 22), b""):
            h.update(block)
    return h.hexdigest()


def load(piimb, subset="sentences", check=True):
    """[{uid, task, text, gold: [(char a, char b)]}] in file order."""
    name = FILES[subset]
    path = os.path.join(piimb, name) if os.path.isdir(piimb) else piimb
    if not os.path.exists(path):
        sys.exit(f"no {name} in {piimb}: download PIIMB (see benchmarks/piimb/README.md)")
    if check and _sha256(path) != SHA256[name]:
        print(f"WARNING: {name} is not the copy these results were computed on (revision {REVISION[:12]}): the "
              f"figures are not comparable", file=sys.stderr)
    rows = []
    with open(path, encoding="utf-8") as f:
        for ln in f:
            if ln.strip():
                r = json.loads(ln)
                rows.append(dict(uid=r["uid"], task=r["task_name"], text=r["text"],
                                 gold=[(int(e["start"]), int(e["end"])) for e in r["entities"]]))
    return rows


def input_name(n):
    return os.path.join(f"{n // SHARD:04d}", f"{n}.txt")


# ---------------------------------------------------------------------------------------------------- the metric
def merge(intervals):
    """Overlapping or touching intervals merged (PIIMB's `_merge_intervals`)."""
    out = []
    for a, b in sorted(intervals):
        if out and a <= out[-1][1]:
            out[-1][1] = max(out[-1][1], b)
        else:
            out.append([a, b])
    return out


def shared_length(A, B):
    total = 0
    for a, b in A:
        for x, y in B:
            lo, hi = max(a, x), min(b, y)
            if hi > lo:
                total += hi - lo
    return total


def f_beta(p, r, beta):
    return (1 + beta * beta) * p * r / (beta * beta * p + r) if p + r else 0.0


class Scorer:
    """Per task: predicted, gold and shared characters, and characters in all."""

    def __init__(self):
        self.acc = {}

    def add(self, task, n_chars, gold, pred):
        a = self.acc.setdefault(task, [0, 0, 0, 0, 0])
        G, P = merge(gold), merge(pred)
        a[0] += sum(b - x for x, b in P)
        a[1] += sum(b - x for x, b in G)
        a[2] += shared_length(P, G)
        a[3] += n_chars
        a[4] += 1

    def result(self):
        out = {}
        for t in sorted(self.acc):
            p_, g_, s_, n_, k_ = self.acc[t]
            pr = s_ / p_ if p_ else 0.0
            rc = s_ / g_ if g_ else 0.0
            out[t] = dict(precision=pr, recall=rc, f1=f_beta(pr, rc, 1), f2=f_beta(pr, rc, 2),
                          fpr=(p_ - s_) / (n_ - g_) if n_ > g_ else 0.0, support_chars=g_, predicted_chars=p_,
                          sentences=k_)
        done = [t for t in TASKS if t in out]
        if done:
            out["_mean"] = {k: sum(out[t][k] for t in done) / len(done) for k in ("precision", "recall", "f1", "f2")}
            out["_mean"]["tasks"] = len(done)
        return out


def byte_to_char(text, spans):
    """[(byte a, byte b)] of the UTF-8 text -> [(char a, char b)], rounding outwards."""
    if text.isascii():
        return [(a, b) for a, b in spans]
    cum = [0]
    for c in text:
        cum.append(cum[-1] + len(c.encode("utf-8")))
    import bisect
    return [(max(0, bisect.bisect_right(cum, a) - 1), min(len(text), bisect.bisect_left(cum, b))) for a, b in spans]


# ---------------------------------------------------------------------------------------------------- reports
def read_cli(path, index):
    """{sentence number: [(byte a, byte b)]} from the CLI's `scan --format jsonl` report(s) over the inputs directory
    (any file of the report whose name is `<n>.txt` is sentence n). Findings are the spans the profile reports."""
    pred, failed, seen = {}, [], set()
    with open(path, encoding="utf-8", errors="replace") as f:
        for ln in f:
            ln = ln.strip()
            if not ln.startswith("{"):
                continue
            r = json.loads(ln)
            name = os.path.basename(str(r.get("file", "")).replace("\\", "/"))
            if not name.endswith(".txt") or not name[:-4].isdigit():
                continue
            n = int(name[:-4])
            if n not in index:
                continue
            if r.get("not_scanned"):
                failed.append(n)
                continue
            seen.add(n)
            items = r.get("findings")
            if items is None:
                items = r.get("spans") or []
            pred.setdefault(n, []).extend((int(x["start"]), int(x["end"])) for x in items)
    return pred, dict(scanned=len(seen), not_scanned=len(failed))


def read_spans(path, uid_to_ns):
    """{sentence number: [(char a, char b)]} from JSON lines {"uid": ..., "spans": [[char a, char b], ...]}. A uid that
    the benchmark repeats (PIIMB has such sentences) is matched in order: its k-th line goes to its k-th sentence."""
    pred, seen = {}, {}
    with open(path, encoding="utf-8") as f:
        for ln in f:
            if ln.strip():
                r = json.loads(ln)
                ns = uid_to_ns.get(r["uid"], [])
                k = seen.get(r["uid"], 0)
                seen[r["uid"]] = k + 1
                if k < len(ns):
                    pred.setdefault(ns[k], []).extend((int(a), int(b)) for a, b, *_ in r["spans"])
    return pred


def score(rows, pred, chars=False, exclude=()):
    sc = Scorer()
    for n, r in enumerate(rows):
        if r["uid"] in exclude:
            continue
        p = pred.get(n, [])
        sc.add(r["task"], len(r["text"]), r["gold"], p if chars else byte_to_char(r["text"], p))
    return sc.result()


def table(name, res):
    lines = [f"### {name}", "", "| Task | Sentences | Precision | Recall | F1 | F2 | FPR |",
             "|---|---:|---:|---:|---:|---:|---:|"]
    for t in TASKS:
        if t in res:
            m = res[t]
            lines.append(f"| {t} | {m['sentences']:,} | {m['precision']:.4f} | {m['recall']:.4f} | {m['f1']:.4f} | "
                         f"**{m['f2']:.4f}** | {m['fpr']:.5f} |")
    if "_mean" in res:
        m = res["_mean"]
        lines.append(f"| **mean of {m['tasks']}** | | {m['precision']:.4f} | {m['recall']:.4f} | {m['f1']:.4f} | "
                     f"**{m['f2']:.4f}** | |")
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------------------------------- commands
def cmd_inputs(a):
    rows = load(a.piimb, a.subset)
    n_written = 0
    for n, r in enumerate(rows):
        p = os.path.join(a.out, "inputs", input_name(n))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        data = r["text"].encode("utf-8")
        if not (os.path.exists(p) and os.path.getsize(p) == len(data)):
            with open(p, "wb") as f:
                f.write(data)
            n_written += 1
    with open(os.path.join(a.out, "index.json"), "w", encoding="utf-8") as f:
        json.dump(dict(subset=a.subset, sentences=len(rows), shard=SHARD,
                       per_task={t: sum(1 for r in rows if r["task"] == t) for t in TASKS}), f, indent=1)
    print(f"{len(rows):,} sentences -> {os.path.join(a.out, 'inputs')} ({n_written:,} files written)")


def cmd_score(a):
    rows = load(a.piimb, a.subset)
    index = set(range(len(rows)))
    uid_to_ns = {}
    for n, r in enumerate(rows):
        uid_to_ns.setdefault(r["uid"], []).append(n)
    exclude = set()
    if a.exclude:
        with open(a.exclude, encoding="utf-8") as f:
            exclude = set(json.load(f)["uids"])
    n_excluded = sum(len(uid_to_ns.get(u, ())) for u in exclude)  # sentences, repeated uids included
    out, text = {}, []
    reports = [(x, False) for x in a.report or []] + [(x, True) for x in a.spans or []]
    if not reports:
        sys.exit("give at least one --report NAME=FILE (CLI) or --spans NAME=FILE (character spans)")
    for spec, chars in reports:
        name, _, path = spec.rpartition("=")
        name = name or os.path.splitext(os.path.basename(path))[0]
        if chars:
            pred, info = read_spans(path, uid_to_ns), {}
        else:
            pred, info = read_cli(path, index)
            if info["not_scanned"]:
                print(f"WARNING: {name}: {info['not_scanned']} input(s) were not scanned (counted as no prediction)",
                      file=sys.stderr)
        res = score(rows, pred, chars)
        out[name] = dict(all=res, report=info)
        text.append(table(f"{name}: every sentence of the `{a.subset}` subset", res))
        if exclude:
            res_x = score(rows, pred, chars, exclude)
            out[name]["without_excluded"] = res_x
            text.append(table(f"{name}: without the {n_excluded:,} sentences of "
                              f"{os.path.basename(a.exclude)}", res_x))
    print("\n".join(text))
    if a.json:
        excluded = dict(file=os.path.basename(a.exclude), sentences=n_excluded) if exclude else None
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(dict(subset=a.subset, revision=REVISION, excluded=excluded, models=out), f, indent=1)


def cmd_trivial(a):
    rows = load(a.piimb, a.subset)
    everything = {n: [(0, len(r["text"]))] for n, r in enumerate(rows)}
    print(table("mask every character", score(rows, everything, chars=True)))
    print(table("mask nothing", score(rows, {}, chars=True)))


def _metrics_of(res):
    """The metric names of the pii recipe's criteria (`pbtrain eval --record piimb`)."""
    out = dict(f2_mean6=res["_mean"]["f2"], precision_mean6=res["_mean"]["precision"],
               recall_mean6=res["_mean"]["recall"], tasks=res["_mean"]["tasks"])
    for t in TASKS:
        if t in res:
            k = t.replace("-", "_")
            out[f"f2_{k}"] = res[t]["f2"]
            out[f"precision_{k}"] = res[t]["precision"]
            out[f"recall_{k}"] = res[t]["recall"]
    return out


def _first_model(path, which="all"):
    with open(path, encoding="utf-8") as f:
        d = json.load(f)
    m = next(iter(d["models"].values()))
    return m[which] if which in m else None


def cmd_metrics(a):
    """{metric: value}: the released file's own figures, and per seed / mean / min / max over the seeds."""
    out = {}
    if a.released:
        rel = _metrics_of(_first_model(a.released))
        out.update({k: round(v, 4) if isinstance(v, float) else v for k, v in rel.items()})
    seeds = [_metrics_of(_first_model(p)) for p in a.seeds or []]
    if seeds:
        for k in seeds[0]:
            if k == "tasks":
                continue
            vals = [s[k] for s in seeds if k in s]
            out[f"{k}_seeds"] = [round(v, 4) for v in vals]
            out[f"{k}_seeds_mean"] = round(sum(vals) / len(vals), 4)
            out[f"{k}_seeds_min"] = round(min(vals), 4)
            out[f"{k}_seeds_max"] = round(max(vals), 4)
        out["n_seeds"] = len(seeds)
    if a.point:
        out = {f"{k}_{a.point}": v for k, v in out.items()}
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1)
    print(json.dumps(out, indent=1))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("inputs", "score", "trivial"):
        p = sub.add_parser(name)
        p.add_argument("--piimb", required=True, help="the folder holding test_sentences.jsonl (or the file itself)")
        p.add_argument("--subset", default="sentences", choices=sorted(FILES))
        if name == "inputs":
            p.add_argument("--out", required=True)
        if name == "score":
            p.add_argument("--inputs", help="the folder `inputs` wrote (for --report)")
            p.add_argument("--report", action="append", help="NAME=FILE: the CLI's `scan --format jsonl` report")
            p.add_argument("--spans", action="append", help='NAME=FILE: JSON lines {"uid", "spans": [[a, b], ...]}, '
                                                             "character offsets")
            p.add_argument("--exclude", help="a JSON object with `uids`: also score without those sentences")
            p.add_argument("--json", help="write every figure to this file")
    m = sub.add_parser("metrics")
    m.add_argument("--released", help="the scores (--json of `score`) of the released file")
    m.add_argument("--seeds", nargs="*", help="the scores of every seed's file")
    m.add_argument("--point", help="a suffix for every metric name (e.g. piimb_mode for another operating point)")
    m.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    {"inputs": cmd_inputs, "score": cmd_score, "trivial": cmd_trivial, "metrics": cmd_metrics}[a.cmd](a)
    return 0


if __name__ == "__main__":
    sys.exit(main())
