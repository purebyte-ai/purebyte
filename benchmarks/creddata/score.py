#!/usr/bin/env python3
"""Score secret scanners on CredData with a line-level metric and a by-repository DEV/TEST split.

Standard library only (Python 3.8+). Nothing here prints, stores or sends a credential value: the scorer reads line
numbers and byte offsets from each tool's report, and the dataset files only to count lines.

Subcommands:

    split    print the DEV/TEST split summary
    files    list the labelled files of a partition (the files to scan)
    regions  write the exact scan regions of a partition (a much faster scan with identical labelled results)
    score    score one or more reports

Examples:

    python3 score.py split  --creddata CredData
    python3 score.py files  --creddata CredData --partition test > files.txt
    python3 score.py regions --creddata CredData --partition test --out regions/
    python3 score.py score  --creddata CredData --purebyte secrets-code=purebyte.jsonl --regions regions/ \
                            --gitleaks gitleaks.json --by-category

The metric (the CredData line rule on a split that no released model was trained or tuned on):

* Unit: one labelled line range, the official meta key (FilePath, LineStart, LineEnd). A unit is positive (T) if
  any meta row on that key is T; otherwise it is negative (F or X).
* A positive unit is a true positive when any flagged line falls inside [LineStart, LineEnd].
* A negative unit is a false positive when a flagged line falls inside it and inside no positive unit. Each unit
  counts once. Flagged lines outside every labelled unit are reported apart and do not count (the official
  benchmark does not count them either).
* 95 % confidence interval of F1: 1,000 bootstrap resamples of repositories, the unit of the split. Precision,
  recall, F1 and the interval are rounded only when printed.
* `score --paired A,B` also prints F1(B) - F1(A) with a paired 95 % interval: the same resamples of repositories
  score both reports, so the interval is that of the difference. `--drop-lines B=TSV` scores report B without the
  labelled units that hold a listed line: give one report twice, under two names, to measure what those lines change.

The split: 50/50 by repository with seed 20260922. Repositories are sorted by their number of positive units, taken
in consecutive pairs, and a seeded coin sends one member of each pair to DEV and the other to TEST. It is computed
from the official meta files, so everyone with the same CredData snapshot gets the same split.

Excluding repositories: `--exclude-repo ID[,ID...]` (any subcommand, repeatable) leaves those repositories out of
every partition, after the split is computed (the split itself never changes). `--exclude-repo training-overlap`
leaves out the four TEST repositories that were also in the code corpus of the first released secrets-code models
(see benchmarks/RESULTS.md).
"""
import argparse
import base64
import collections
import csv
import gzip
import hashlib
import json
import os
import random
import re
import sys

SPLIT_SEED = 20260922
BOOT_SEED = 7
BOOT_SAMPLES = 1000
MAX_BYTES = 4_000_000       # the text scanner's per-file limit; headline results use files up to this size
OPENSSL_REPO = "2ba83c6a"   # openssl/openssl in CredData: cryptographic test vectors, about a third of all T lines
WINDOW, STRIDE, MARGIN = 512, 384, 768   # the scanner's windows; regions keep two strides of context on each side
# TEST repositories that were also in the code corpus of the first released secrets-code models (django, kubernetes,
# curl, ohmyzsh): `--exclude-repo training-overlap` leaves them out, as the published TEST figures do.
REPO_SETS = {"training-overlap": ("f710ac3c", "fc8343f4", "e13d78bf", "c8aa9b49")}

PARTITIONS = {
    "test": lambda s, repo: s[repo] == "test",
    "dev": lambda s, repo: s[repo] == "dev",
    "dev-no-openssl": lambda s, repo: s[repo] == "dev" and repo != OPENSSL_REPO,
    "all": lambda s, repo: True,
    "all-no-openssl": lambda s, repo: repo != OPENSSL_REPO,
}


# ------------------------------------------------------------------------------------------------------ dataset
class CredData:
    """The official meta files, the labelled units, the split and the file sizes of a CredData checkout."""

    def __init__(self, root, exclude=()):
        self.root = os.path.abspath(root)
        meta = os.path.join(self.root, "meta")
        if not os.path.isdir(meta):
            sys.exit(f"no meta/ directory in {self.root}: --creddata must be the CredData checkout")
        self.rows = []
        for name in sorted(os.listdir(meta)):
            if name.endswith(".csv"):
                with open(os.path.join(meta, name), encoding="utf-8", newline="") as fh:
                    self.rows.extend(csv.DictReader(fh))
        if not self.rows:
            sys.exit(f"no meta rows in {meta}")
        self.all_units = self._units(self.rows)
        self.split = self._split(self.rows, self.all_units)
        # Left out of every partition after the split, so that the split itself does not change.
        self.excluded = sorted(set(exclude))
        unknown = [r for r in self.excluded if r not in self.split]
        if unknown:
            sys.exit(f"--exclude-repo: no repository {', '.join(unknown)} in this CredData checkout")
        # Labelled files missing on disk (for example removed by an antivirus) leave the evaluation.
        self.units = {k: u for k, u in self.all_units.items() if os.path.exists(self.path(k[0]))}
        self.missing_files = len({k[0] for k in self.all_units}) - len({k[0] for k in self.units})
        self.size = {}
        for k in self.units:
            if k[0] not in self.size:
                self.size[k[0]] = os.path.getsize(self.path(k[0]))

    def path(self, meta_path):
        return os.path.join(self.root, meta_path)

    @staticmethod
    def _units(rows):
        by_key = collections.defaultdict(list)
        for r in rows:
            by_key[(r["FilePath"], int(r["LineStart"]), int(r["LineEnd"]))].append(r)
        units = {}
        for key, rs in by_key.items():
            labels = {r["GroundTruth"] for r in rs}
            label = "T" if "T" in labels else ("F" if "F" in labels else "X")
            cats = sorted({c for r in rs if r["GroundTruth"] == label for c in r["Category"].split(":") if c})
            units[key] = dict(label=label, cats=cats, repo=rs[0]["RepoName"])
        return units

    @staticmethod
    def _split(rows, units, seed=SPLIT_SEED):
        n_pos = collections.Counter(u["repo"] for u in units.values() if u["label"] == "T")
        repos = sorted({r["RepoName"] for r in rows})
        order = sorted(repos, key=lambda r: (-n_pos[r], hashlib.sha256(f"{seed}:{r}".encode()).hexdigest()))
        rng = random.Random(seed)
        split = {}
        for i in range(0, len(order), 2):
            pair = order[i:i + 2]
            rng.shuffle(pair)
            split[pair[0]] = "dev"
            if len(pair) > 1:
                split[pair[1]] = "test"
        return split

    def in_partition(self, part, unit):
        return unit["repo"] not in self.excluded and PARTITIONS[part](self.split, unit["repo"])

    def label(self, part):
        """The partition's name, with the excluded repositories if any."""
        return f"{part} without {', '.join(self.excluded)}" if self.excluded else part

    def files(self, part, max_bytes=MAX_BYTES):
        """Labelled, non-empty files of a partition, up to `max_bytes` (None: any size)."""
        return sorted({k[0] for k, u in self.units.items() if self.in_partition(part, u)
                       and 0 < self.size[k[0]] and (max_bytes is None or self.size[k[0]] <= max_bytes)})


def macro_category(cats):
    """Coarse family of a unit, from its official categories."""
    s = " ".join(cats).lower()
    if "private key" in s or "jwk" in s or "pkcs" in s:
        return "private_key"
    if "password" in s or "passwd" in s or "securestring" in s or "curl" in s:
        return "password"
    if "url credentials" in s or "basic authorization" in s:
        return "url_or_basic_auth"
    generic = {"Key", "Secret", "API", "Token", "Auth", "Credential", "Salt", "Nonce", "UUID", "Bearer Authorization",
               "JSON Web Token", "OTP / 2FA Secret", "Other"}
    if any(c not in generic for c in cats):
        return "provider_pattern"
    if any(c in ("Token", "Auth", "Bearer Authorization", "JSON Web Token") for c in cats):
        return "token_or_auth"
    if any(c in ("Key", "Secret", "API", "Credential", "OTP / 2FA Secret") for c in cats):
        return "generic_key_or_secret"
    if any(c in ("UUID", "Salt", "Nonce") for c in cats):
        return "uuid_salt_nonce"
    return "other"


# ------------------------------------------------------------------------------------------------------ regions
def line_starts(data):
    return [0] + [m.end() for m in re.finditer(b"\n", data)]


def scan_regions(data, line_ranges, window=WINDOW, stride=STRIDE, margin=MARGIN):
    """Byte ranges that give the scanner exactly the windows it would see in the whole file around each labelled
    line: a region starts at a multiple of the stride and ends at a window end (or at the end of the file), with
    `margin` bytes of context on each side. Windows are independent, so findings on labelled lines are identical."""
    n = len(data)
    starts = line_starts(data)
    regs = []
    for first, last in line_ranges:
        if first - 1 >= len(starts):
            continue
        a = max(0, starts[first - 1] - margin)
        z = min(n, (starts[last] if last < len(starts) else n) + margin)
        a = (a // stride) * stride
        k = max(0, -(-(z - window) // stride))
        z = min(n, k * stride + window)
        regs.append([a, z])
    regs.sort()
    merged = []
    for a, z in regs:
        if merged and a <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], z)
        else:
            merged.append([a, z])
    return merged


def cmd_regions(cd, a):
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    ranges = collections.defaultdict(list)
    for (f, first, last) in cd.units:
        ranges[f].append((first, last))
    listing, mapping, whole, region_bytes = [], {}, 0, 0
    for f in cd.files(a.partition):
        with open(cd.path(f), "rb") as fh:
            data = fh.read()
        regs = scan_regions(data, ranges[f])
        if len(regs) == 1 and regs[0] == [0, len(data)]:
            listing.append(cd.path(f))
            whole += 1
            region_bytes += len(data)
            continue
        ext = os.path.splitext(f)[1]
        for lo, hi in regs:
            name = f"{f.replace('/', '__')}.{lo}{ext}"
            with open(os.path.join(out, name), "wb") as fh:
                fh.write(data[lo:hi])
            mapping[name] = [f, lo, data.count(b"\n", 0, lo)]
            listing.append(os.path.join(out, name))
            region_bytes += hi - lo
    with open(os.path.join(out, "regions.json"), "w", encoding="utf-8") as fh:
        json.dump(dict(partition=a.partition, excluded_repos=cd.excluded, window=WINDOW, stride=STRIDE, margin=MARGIN,
                       regions=mapping), fh)
    with open(os.path.join(out, "files.txt"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(listing) + "\n")
    print(f"{cd.label(a.partition)}: {len(listing)} inputs to scan ({len(mapping)} regions, {whole} whole files), "
          f"{region_bytes / 2**20:.1f} MiB; list in {os.path.join(out, 'files.txt')}")


# ------------------------------------------------------------------------------------------------------ reports
def _open(path):
    return gzip.open(path, "rt", encoding="utf-8") if path.endswith(".gz") else open(path, encoding="utf-8")


def _json_items(path):
    """The JSON values of a file: one document, JSON Lines, or several documents appended one after another."""
    with _open(path) as fh:
        text = fh.read()
    decoder, items, i = json.JSONDecoder(), [], 0
    while True:
        while i < len(text) and text[i].isspace():
            i += 1
        if i == len(text):
            return items
        value, i = decoder.raw_decode(text, i)
        items.append(value)


class Locator:
    """Maps report paths to CredData meta paths (`data/<repo>/<scope>/<file>`), undoing scan regions if any."""

    def __init__(self, cd, regions_dir=None):
        self.cd = cd
        self.regions_dir = os.path.abspath(regions_dir) if regions_dir else None
        self.regions = {}
        if regions_dir:
            with open(os.path.join(self.regions_dir, "regions.json"), encoding="utf-8") as fh:
                self.regions = json.load(fh)["regions"]

    def locate(self, p):
        """(meta path, byte offset of the scanned input in that file, line offset, path of the scanned input)."""
        p = p.replace("\\", "/")
        base = p.rsplit("/", 1)[-1]
        if base in self.regions:
            f, lo, line_off = self.regions[base]
            return f, lo, line_off, os.path.join(self.regions_dir, base)
        root = self.cd.root
        for c in ([p] if os.path.isabs(p) else [os.path.join(root, p), os.path.join(root, "data", p), os.path.abspath(p)]):
            if os.path.exists(c):
                f = os.path.relpath(c, root).replace("\\", "/")
                return f, 0, 0, c
        i = p.find("data/")
        f = p[i:] if i >= 0 else p.lstrip("./")
        return f, 0, 0, os.path.join(root, f)


def _read(path, cache):
    if cache.get("path") != path:
        try:
            with open(path, "rb") as fh:
                cache.update(path=path, data=fh.read())
        except OSError:
            cache.update(path=path, data=None)
    return cache["data"]


def _finding_lines(f, data):
    """Number of lines a PureByte finding covers: `end_line` when the report has it; otherwise the newlines of the
    flagged bytes (for a finding inside a base64 blob, of the decoded bytes) plus one."""
    if f.get("end_line") is not None:
        return max(1, int(f["end_line"]) - int(f["line"]) + 1)
    start, end = f.get("start"), f.get("end")
    if data is None or start is None or end is None:
        return 1
    chunk = data[int(start):int(end)]
    if f.get("inside_base64") and f.get("inner_start") is not None and f.get("inner_end") is not None:
        try:
            chunk = base64.b64decode(b"".join(chunk.split()))[int(f["inner_start"]):int(f["inner_end"])]
        except ValueError:
            pass
    return chunk.count(b"\n") + 1


def load_purebyte(path, loc):
    findings = []
    for item in _json_items(path):
        if isinstance(item, dict) and isinstance(item.get("findings"), list):
            findings.extend(item["findings"])
        elif isinstance(item, list):
            findings.extend(item)
        elif isinstance(item, dict) and "line" in item:
            findings.append(item)
    per, cache = collections.defaultdict(list), {}
    for f in findings:
        meta, _, line_off, scanned = loc.locate(f.get("file") or f.get("path") or "")
        per[meta].append(dict(l=int(f["line"]) + line_off, n=_finding_lines(f, _read(scanned, cache))))
    return per


def load_gitleaks(path, loc):
    per = collections.defaultdict(list)
    for item in _json_items(path):
        for x in (item if isinstance(item, list) else [item]):
            meta = loc.locate(x["File"])[0]
            per[meta].append(dict(l=int(x["StartLine"]), n=int(x["EndLine"]) - int(x["StartLine"]) + 1))
    return per


def load_trufflehog(path, loc):
    """One line per finding. Whether trufflehog's line numbers are 0- or 1-based is decided from the data, in memory,
    by looking for the start of the raw value on the reported line (the value is never printed or kept)."""
    per, votes, cache = collections.defaultdict(list), collections.Counter(), {}
    for x in _json_items(path):
        try:
            fs = x["SourceMetadata"]["Data"]["Filesystem"]
        except (KeyError, TypeError):
            continue
        meta, _, _, scanned = loc.locate(fs["file"])
        line = int(fs.get("line", 0))
        raw = (x.get("Raw") or "").split("\n")[0].strip()[:20]
        data = _read(scanned, cache)
        lines = data.decode("utf-8", "replace").split("\n") if data is not None else []
        if raw and 1 <= line <= len(lines) and raw in lines[line - 1]:
            votes["1-based"] += 1
        elif raw and 0 <= line < len(lines) and raw in lines[line]:
            votes["0-based"] += 1
        per[meta].append(dict(l=line, n=1))
    shift = 1 if votes["0-based"] > votes["1-based"] else 0
    for flags in per.values():
        for d in flags:
            d["l"] += shift
    return per


def load_flags(path, loc):
    per = collections.defaultdict(list)
    for item in _json_items(path):
        if isinstance(item, dict) and "f" in item:
            meta, _, line_off, _ = loc.locate(item["f"])
            per[meta].extend(dict(l=int(d["l"]) + line_off, n=int(d.get("n", 1))) for d in item.get("flags", []))
    return per


LOADERS = {"purebyte": load_purebyte, "gitleaks": load_gitleaks, "trufflehog": load_trufflehog, "flags": load_flags}


# ------------------------------------------------------------------------------------------------------ scoring
def score(units, flags, keep):
    """(true-positive units, false-positive units, positive units, negative units, flagged lines outside labels)."""
    pos, neg = collections.defaultdict(list), collections.defaultdict(list)
    for k, u in units.items():
        if keep(k, u):
            (pos if u["label"] == "T" else neg)[k[0]].append(k)
    tp, fp, outside = set(), set(), 0
    for f in set(pos) | set(neg):
        lines = set()
        for x in flags.get(f, []):
            lines.update(range(x["l"], x["l"] + max(1, x.get("n", 1))))
        for line in lines:
            in_pos = [k for k in pos[f] if k[1] <= line <= k[2]]
            if in_pos:
                tp.update(in_pos)
                continue
            in_neg = [k for k in neg[f] if k[1] <= line <= k[2]]
            if in_neg:
                fp.update(in_neg)
            else:
                outside += 1
    return tp, fp, [k for ks in pos.values() for k in ks], [k for ks in neg.values() for k in ks], outside


def prf(tp, fp, fn):
    """Precision, recall and F1, unrounded: they are rounded once, when printed."""
    p = tp / (tp + fp) if tp + fp else 0.0
    r = tp / (tp + fn) if tp + fn else 0.0
    return p, r, (2 * p * r / (p + r) if p + r else 0.0)


def per_repo(units, tp, fp, positives):
    """repository -> [true positives, false positives, false negatives]: the unit of the bootstrap."""
    per = collections.defaultdict(lambda: [0, 0, 0])
    for k in positives:
        per[units[k]["repo"]][0 if k in tp else 2] += 1
    for k in fp:
        per[units[k]["repo"]][1] += 1
    return per


def f1_of(per, repos):
    s = [0, 0, 0]
    for repo in repos:
        a = per.get(repo, (0, 0, 0))
        s[0] += a[0]
        s[1] += a[1]
        s[2] += a[2]
    return prf(*s)[2]


def bootstrap_f1(units, tp, fp, positives, seed=BOOT_SEED, n=BOOT_SAMPLES):
    """95 % interval of F1, resampling repositories with replacement."""
    per = per_repo(units, tp, fp, positives)
    repos = sorted(per)
    if not repos:
        return [0.0, 0.0]
    rng = random.Random(seed)
    f1s = []
    for _ in range(n):
        f1s.append(f1_of(per, [repos[rng.randrange(len(repos))] for _ in repos]))
    f1s.sort()
    return [f1s[int(0.025 * n)], f1s[int(0.975 * n) - 1]]


def paired_f1_difference(per_a, per_b, seed=BOOT_SEED, n=BOOT_SAMPLES):
    """F1(B) - F1(A) and its 95 % paired interval: every resample of repositories scores A and B together, so the
    interval is that of the difference, not of either F1. Returns (difference, [low, high], repositories)."""
    repos = sorted(set(per_a) | set(per_b))
    if not repos:
        return 0.0, [0.0, 0.0], 0
    rng = random.Random(seed)
    diffs = []
    for _ in range(n):
        sample = [repos[rng.randrange(len(repos))] for _ in repos]
        diffs.append(f1_of(per_b, sample) - f1_of(per_a, sample))
    diffs.sort()
    return f1_of(per_b, repos) - f1_of(per_a, repos), [diffs[int(0.025 * n)], diffs[int(0.975 * n) - 1]], len(repos)


def load_lines(path):
    """(file, line) pairs from a TSV of `data/<repo>/<scope>/<file>` and a 1-based line number per row."""
    out = set()
    with open(path, encoding="utf-8") as fh:
        for row in fh:
            parts = row.rstrip("\n").split("\t")
            if len(parts) >= 2 and parts[1].strip().isdigit():
                out.add((parts[0], int(parts[1])))
    return out


SCOPES = {
    "files <= 4 MB": lambda cd, k, u: cd.size[k[0]] <= MAX_BYTES,
    "files <= 4 MB, without UUID/salt/nonce-only units":
        lambda cd, k, u: cd.size[k[0]] <= MAX_BYTES and macro_category(u["cats"]) != "uuid_salt_nonce",
    "all file sizes": lambda cd, k, u: True,
}


def cmd_score(cd, a):
    loc = Locator(cd, a.regions)
    reports = []
    for kind, loader in LOADERS.items():
        for spec in getattr(a, kind):
            name, sep, path = spec.partition("=") if "=" in spec and not os.path.exists(spec) else ("", "", spec)
            reports.append((name or f"{kind}:{os.path.basename(path)}", loader(path, loc)))
    if not reports:
        sys.exit("nothing to score: pass at least one report (--purebyte, --gitleaks, --trufflehog or --flags)")
    names = [name for name, _ in reports]
    dropped = {}                                   # report name -> the units it is scored without
    for spec in a.drop_lines:
        name, sep, path = spec.partition("=")
        if not sep or name not in names:
            sys.exit(f"--drop-lines {spec}: expected NAME=TSV with NAME one of the reports ({', '.join(names)})")
        lines = load_lines(path)
        dropped[name] = {k for k in cd.units if any((k[0], line) in lines for line in range(k[1], k[2] + 1))}
        print(f"{name}: scored without the {len(dropped[name])} labelled units that hold a line of {path}")
    paired = None
    if a.paired:
        paired = [x.strip() for x in a.paired.split(",")]
        if len(paired) != 2 or any(x not in names for x in paired):
            sys.exit(f"--paired {a.paired}: expected A,B, two of the reports ({', '.join(names)})")
    results = {}
    for part in a.partitions:
        for scope_name, in_scope in SCOPES.items():
            print(f"\n{cd.label(part)} · {scope_name}\n")
            print("| tool | T | TP | FP | FN | P | R | F1 | F1 95 % CI (by repo) |")
            print("|---|---:|---:|---:|---:|---:|---:|---:|---|")
            pers = {}
            for name, flags in reports:
                keep = (lambda k, u, p=part, s=in_scope, d=dropped.get(name, ()): cd.in_partition(p, u) and s(cd, k, u)
                        and k not in d)
                tp, fp, positives, negatives, outside = score(cd.units, flags, keep)
                n_tp, n_fp, n_fn = len(tp), len(fp), len(positives) - len(tp)
                P, R, F1 = prf(n_tp, n_fp, n_fn)
                ci = bootstrap_f1(cd.units, tp, fp, positives)
                if paired and name in paired:
                    pers[name] = per_repo(cd.units, tp, fp, positives)
                print(f"| {name} | {len(positives)} | {n_tp} | {n_fp} | {n_fn} | {P:.3f} | {R:.3f} | {F1:.3f} "
                      f"| {ci[0]:.3f}-{ci[1]:.3f} |")
                row = dict(T=len(positives), negatives=len(negatives), TP=n_tp, FP=n_fp, FN=n_fn, P=P, R=R, F1=F1,
                           F1_ci95=ci, flagged_lines_outside_labels=outside)
                if a.by_category:
                    fam = collections.defaultdict(lambda: [0, 0, 0])
                    for k in positives:
                        fam[macro_category(cd.units[k]["cats"])][0 if k in tp else 2] += 1
                    for k in fp:
                        fam[macro_category(cd.units[k]["cats"])][1] += 1
                    row["by_category"] = {c: dict(T=v[0] + v[2], TP=v[0], FP=v[1], P=prf(*v)[0], R=prf(*v)[1])
                                          for c, v in sorted(fam.items(), key=lambda x: -(x[1][0] + x[1][2])) if v != [0, 0, 0]}
                results[f"{part} | {scope_name} | {name}"] = row
            if paired:
                d, dci, n_rep = paired_f1_difference(pers[paired[0]], pers[paired[1]])
                print(f"\npaired: F1({paired[1]}) - F1({paired[0]}) = {d:+.5f}, 95 % paired interval {dci[0]:+.5f} to "
                      f"{dci[1]:+.5f} ({BOOT_SAMPLES} resamples of {n_rep} repositories, the same ones for both)")
                results[f"{part} | {scope_name} | paired {paired[0]} -> {paired[1]}"] = dict(
                    F1_difference=d, ci95=dci, repositories=n_rep, resamples=BOOT_SAMPLES)
            if a.by_category:
                print()
                for name, _ in reports:
                    cats = results[f"{part} | {scope_name} | {name}"]["by_category"]
                    print(f"- {name}: " + ", ".join(f"{c} R {v['R']:.2f} P {v['P']:.2f} (T {v['T']})" for c, v in cats.items()))
    if a.json:
        with open(a.json, "w", encoding="utf-8") as fh:
            json.dump(dict(split_seed=SPLIT_SEED, max_bytes=MAX_BYTES, excluded_repos=cd.excluded, results=results), fh,
                      indent=1)
        print(f"\nwrote {a.json}")


def cmd_split(cd, a):
    for part in a.partitions:
        files = {k[0] for k, u in cd.units.items() if cd.in_partition(part, u)}
        small = {f for f in files if cd.size[f] <= MAX_BYTES}
        repos = {u["repo"] for u in cd.units.values() if cd.in_partition(part, u)}
        n_t = sum(1 for k, u in cd.units.items() if cd.in_partition(part, u) and u["label"] == "T"
                  and cd.size[k[0]] <= MAX_BYTES)
        print(f"{cd.label(part):15s} {len(repos):4d} repositories · {len(files):5d} labelled files "
              f"({sum(cd.size[f] for f in files) / 2**20:6.1f} MiB) · up to 4 MB: {len(small):5d} files "
              f"({sum(cd.size[f] for f in small) / 2**20:6.1f} MiB), {n_t} positive units")
    if a.json:
        with open(a.json, "w", encoding="utf-8") as fh:
            json.dump(dict(seed=SPLIT_SEED, repositories=dict(sorted(cd.split.items()))), fh, indent=1)


def cmd_files(cd, a):
    for f in cd.files(a.partition, None if a.any_size else MAX_BYTES):
        print(f)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    parts = ", ".join(PARTITIONS)
    s = sub.add_parser("split", help="print the DEV/TEST split summary")
    s.add_argument("--partitions", default="test,dev,dev-no-openssl,all", help=f"comma list of: {parts}")
    s.add_argument("--json", metavar="PATH", help="also write the split (repository -> dev/test) to this file")
    f = sub.add_parser("files", help="list the labelled files of a partition, relative to the checkout")
    f.add_argument("--partition", default="test", choices=list(PARTITIONS))
    f.add_argument("--any-size", action="store_true", help="include files over 4 MB")
    r = sub.add_parser("regions", help="write exact scan regions for a partition")
    r.add_argument("--partition", default="test", choices=list(PARTITIONS))
    r.add_argument("--out", required=True, help="empty directory for the region files, files.txt and regions.json")
    c = sub.add_parser("score", help="score reports")
    for kind in LOADERS:
        c.add_argument(f"--{kind}", action="append", default=[], metavar="[NAME=]PATH", help=f"a {kind} report (.gz ok)")
    c.add_argument("--regions", metavar="DIR", help="the directory written by `regions`, if the scan used it")
    c.add_argument("--partitions", default="test,dev-no-openssl", help=f"comma list of: {parts} (default: %(default)s)")
    c.add_argument("--by-category", action="store_true", help="recall and precision per category family")
    c.add_argument("--json", metavar="PATH", help="write every number to this file")
    c.add_argument("--paired", metavar="A,B", help="also print F1(B) - F1(A) with a paired 95 %% interval (the same "
                   "resamples of repositories for both); A and B are two report names")
    c.add_argument("--drop-lines", action="append", default=[], metavar="NAME=TSV",
                   help="score report NAME without the labelled units that hold a line listed in TSV (rows: "
                        "data/<repo>/<scope>/<file>, TAB, line number); give a file twice under two names to compare "
                        "it with and without those lines (repeatable)")
    sets = ", ".join(REPO_SETS)
    for p in (s, f, r, c):
        p.add_argument("--creddata", required=True, help="the CredData checkout (with meta/ and data/)")
        p.add_argument("--exclude-repo", action="append", default=[], metavar="ID[,ID...]",
                       help=f"leave these repositories (CredData ids, or: {sets}) out of every partition")
    a = ap.parse_args(argv)
    if hasattr(a, "partitions"):
        a.partitions = [p.strip() for p in a.partitions.split(",") if p.strip()]
        for p in a.partitions:
            if p not in PARTITIONS:
                ap.error(f"unknown partition `{p}` (choose from {parts})")
    exclude = [x.strip() for spec in a.exclude_repo for x in spec.split(",") if x.strip()]
    exclude = [r for x in exclude for r in REPO_SETS.get(x, (x,))]
    cd = CredData(a.creddata, exclude)
    if a.cmd != "files":
        print(f"CredData: {len(cd.rows)} meta rows, {len(cd.all_units)} labelled units, {len(cd.split)} repositories"
              + (f"; {cd.missing_files} labelled files are missing on disk and left out" if cd.missing_files else "")
              + (f"; left out: {', '.join(cd.excluded)}" if cd.excluded else ""))
    {"split": cmd_split, "files": cmd_files, "regions": cmd_regions, "score": cmd_score}[a.cmd](cd, a)
    return 0


if __name__ == "__main__":
    sys.exit(main())
