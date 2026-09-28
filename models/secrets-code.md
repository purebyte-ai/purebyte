# Model card: secrets-code

An **AI Specialist** that finds credentials (API keys, access tokens, passwords, private keys, connection strings) in
source code, configuration and other text, and reports exactly where they are. It reads raw bytes, runs on ordinary CPUs
and never sends anything anywhere.

It is one of the three example specialists of PureByte 1.0, which show what one byte-level architecture does on different
jobs; the architecture, the runtime and the training stack are described in the [README](../README.md).

| | |
|---|---|
| Version | 1.0.0 |
| Task | Credential detection as byte spans in text files |
| Input | Any text file up to 4,000,000 bytes: UTF-8, or UTF-16 (converted in memory); inputs under 24 bytes are not analyzed |
| Output | Findings: file, line, column, byte offsets, severity, masked snippet, and ensemble votes when an ensemble is used (see [spec/OUTPUT.md](../spec/OUTPUT.md)) |
| Size | One file of 1,028,704 bytes (0.98 MiB); 1,927,520 parameters (1.93 M) in the network: the byte embedding, the blocks and the final norm. The two heads add 34,439 (1,961,959 in all); the per-group scales of the ternary weights are derived, not counted |
| Architecture | Byte-level state-space model with ternary weights (four `ssm_v2` blocks of width 256) and a span-tagging head gated by a window head with max pooling ([docs/architecture.md](../docs/architecture.md)) |
| Default decision rule | One model, at the operating point stored in its file (`--bias` moves it) |
| Optional ensemble | `secrets-code-ensemble`: the same model plus two more trained the same way from other random seeds, each at its own operating point; a finding needs two of the three votes. Three times the work |
| Post-processing | The `secrets-code` profile: it applies format rules, decodes base64 layers, ignores documentation examples, counts the ensemble votes, sets the severity by path and masks the values |
| Runtime | `purebyte` 1.0.0 or newer, on an x86-64 or arm64 CPU (other little-endian CPUs through the portable build); no GPU, no network |
| License | [PureByte Model License](../MODEL_LICENSE.md) (the code of the runtime is Apache-2.0) |
| Files | `purebyte-secrets-code-1.0.0.gguf` (the model) and, for the ensemble, `-e1` and `-e2`; checksums in [models.json](models.json) |
| Recipe | [specialists/secrets-code](https://github.com/purebyte-ai/purebyte-train/blob/main/specialists/secrets-code/README.md) in purebyte-train: the generator, the recipe and the evaluation, to rebuild or improve it |

```bash
purebyte models pull secrets-code                   # the model
purebyte models pull secrets-code-ensemble          # optional: the two extra members of the ensemble
purebyte scan --model secrets-code-ensemble src/
```

## Intended use

- Pre-commit hooks and CI checks on changed files, where a few kilobytes are scanned per commit.
- Local scans of repositories, build artifacts and configuration bundles before they leave the machine.
- A second opinion next to rule-based scanners: it finds credentials that have no fixed pattern, from the context
  around them.
- Small decisions through the HTTP API: "does this line, event or snippet contain a credential?"
- Redacted copies of configuration files and logs (`purebyte redact --model secrets-code`), with each credential
  replaced by a marker such as `[secret_1]`.

## Out of scope

- **Proving that code is free of secrets.** It misses a share of real credentials (below). Use it as one layer of
  defense, never as the only one.
- **Checking whether a credential is live.** It never contacts any service, by design.
- **Binary files.** Use [secrets-bin](secrets-bin.md).
- **Personal data** (e-mail addresses, phone numbers, identity numbers). That is the job of [pii](pii.md).
- **Files over 4,000,000 bytes**, which are reported as not analyzed.

## Evaluation

On the TEST half of [CredData](https://github.com/Samsung/CredData) (a split by repository, computed from the
dataset's metadata; files up to 4 MB; line-level metric), measured together with gitleaks and trufflehog on the same
files. The TEST half has 168 repositories; four of them overlap the code corpus the model was trained with and are
left out ([Training-data overlap](../benchmarks/RESULTS.md#training-data-overlap)): 164 repositories, 4,219 labeled
credential lines.

| Tool | Precision | Recall | F1 [95 % CI] |
|---|---:|---:|---|
| **secrets-code** 1.0.0 | 0.879 | **0.729** | **0.797** [0.719-0.860] |
| secrets-code, ensemble of three (`secrets-code-ensemble`) | 0.879 | 0.741 | 0.805 [0.731-0.863] |
| gitleaks 8.30.1 | 0.905 | 0.207 | 0.337 [0.263-0.421] |
| trufflehog 3.97.6 (no verification) | 0.578 | 0.025 | 0.047 [0.017-0.092] |

On the complete TEST half (all 168 repositories), secrets-code scores precision 0.876, recall 0.659, F1 0.753
[0.665-0.841]. For fewer false alarms, `--bias -2` (an operating point chosen on DEV) gives precision 0.932 and recall
0.663 on the 164 repositories. Counts, other slices, results by category, the operating points and the full reproduction
procedure: [benchmarks/RESULTS.md](../benchmarks/RESULTS.md). The errors on TEST are counted, never inspected.

**How to read these figures.** Besides generated examples, the model learned from labeled lines of CredData
repositories of the DEV half, which share no repository with TEST but follow the same labeling policy (for
example, credential-shaped values in test code count as credentials). Part of the gain over rule-based scanners may
therefore be CredData's own conventions; on repositories labeled differently, expect lower figures.

**On the original values.** CredData replaces every labeled credential value with a random string, and the model
learned from such values in the DEV half. With the original values restored on the same 164 repositories, its recall
is 0.583 instead of 0.729 and its F1 0.694 instead of 0.797 (the ensemble: 0.597 and 0.704); gitleaks barely moves
(recall 0.201, F1 0.329). The model still finds 2.9 times as many labeled credential lines as gitleaks (2,460 against
850). The loss is in passwords that are ordinary words ([details](../benchmarks/RESULTS.md#on-the-original-values)).

## False alarms on repositories without credentials

Three sets of popular public repositories that hold no real credential, pinned by commit
([data/exams/code-fp.yaml](https://github.com/purebyte-ai/purebyte-train/blob/main/data/exams/code-fp.yaml) in
purebyte-train), scanned with the default options; every finding is counted as a false alarm, even when it is
credential-shaped test data:

| Set | Repositories | Files scanned | Findings | Of them, warnings (test, example and documentation paths) |
|---|---:|---:|---:|---:|
| code-fp-a | 6 | 1,219 | 93 | 90 |
| code-fp-a without tornado | 5 | 931 | 73 | 70 |
| code-fp-b | 18 | 4,823 | 408 | 392 |
| code-fp-b without starlette, got and devise | 15 | 4,322 | 339 | 323 |
| code-fp-c | 12 | 4,778 | 138 | 49 |
| code-fp-c without awesome-compose, nginx, helm-charts, dotfiles and home-manager | 7 | 2,046 | 51 | 27 |

Nine of these repositories overlap the training data, so each set is also given without them: tornado, starlette, got,
devise and awesome-compose are CredData DEV repositories whose labeled lines the model learned from, and
awesome-compose, nginx, helm-charts, dotfiles and home-manager were in the code corpus its generated examples were cut
from ([details](../benchmarks/RESULTS.md#false-alarms-on-repositories-without-credentials)). The released model was
also chosen on code-fp-b ([Training data](#training-data)), so its count there is optimistic in either row.

Most findings are warnings: keys, certificates, cookies and `Basic` headers in tests, fixtures and documentation,
which look exactly like credentials and do not fail a scan. The errors (108 across the three complete sets) come
mostly from integrity hashes in `package-lock.json` files, base64 image data embedded in a notebook, demo passwords and
keys of example applications outside test folders, and example `Basic` headers in doc comments.

## Known limitations

- **It misses about one labeled credential in four** on CredData TEST as distributed (recall 0.729), and two in five
  on the original values (recall 0.583), mostly passwords that are ordinary words. The classes it misses most,
  measured on the DEV repositories that were never used in training: identifiers that CredData labels as credentials
  (UUIDs, salts, nonces), found only under an unequivocal credential name, by design; values with no name next to them;
  and short quoted passwords ([known weak spots](../benchmarks/RESULTS.md#known-weak-spots)).
- **Its most common wrong finding is a short ordinary value under a credential-like name** (about 45 % of its false
  positives on the DEV repositories never used in training), then strings full of symbols, and multi-line blocks that
  CredData does not count as private keys.
- **Credential-shaped test data is reported.** Fixtures, example headers and test keys look exactly like credentials;
  in test, example and documentation paths they are warnings (`--tests no` skips those paths).
- **Hashes and base64 data can be flagged**: integrity hashes in lock files and data embedded in notebooks were the
  largest sources of errors on the repositories without credentials (above).
- **Cryptographic test vectors are treated as non-secrets** on purpose (for example `Key = <hex>` in test data).
- **Documentation examples are ignored by default** (for example AWS's `AKIAIOSFODNN7EXAMPLE`); `--keep-examples`
  reports them.
- **Confidence values are not calibrated yet.** Use the finding itself (and the votes, with the ensemble), not the
  number alone.
- **Throughput is modest.** It suits commits, diffs and small decisions better than full-history scans of large
  repositories: about 130 KB/s on a 12-core desktop ([docs/performance.md](../docs/performance.md)).

## Training data

- **Generated examples in real code.** Windows of real code and configuration from public repositories (for the
  released weights, clones made on 19 September 2026; the data scripts rebuild the corpus from repositories pinned by
  commit), each with generated lines injected: credentials in real provider formats, connection strings, private keys,
  generic secrets under credential names, and negatives that only look like credentials (placeholders, environment
  lookups, hashes, identifiers, public keys, documentation examples, ordinary short values). Only the injected values
  are marked.
- **Labeled real lines.** One window in five of every training step comes from lines labeled as credentials or
  non-credentials in CredData repositories of the DEV half: a fixed 117 of its 169 repositories (the others are kept
  apart for measuring, and the openssl repository is left out); no TEST repository was read. Half of those credential
  values are replaced by generated values of the same form.
- **Overlap.** The code corpus of the time held four CredData TEST repositories, which are left out of the published
  figures ([details](../benchmarks/RESULTS.md#training-data-overlap)), and five repositories of the false-alarm set
  code-fp-c; four more false-alarm repositories, and one of those five, are CredData DEV repositories whose labeled
  lines it learned from (the figures without all nine are above). The data scripts of
  [purebyte-train](https://github.com/purebyte-ai/purebyte-train) rebuild the corpus from public sources, excluding
  every CredData repository and every exam repository:
  [data/](https://github.com/purebyte-ai/purebyte-train/blob/main/data/README.md).

The generator and the recipe are in
[specialists/secrets-code](https://github.com/purebyte-ai/purebyte-train/blob/main/specialists/secrets-code/README.md)
of purebyte-train. Three models are trained with different random seeds: one is released as the model, and the other
two are the extra members of the ensemble. **The released one was chosen on the false-alarm set code-fp-b** (the fewest
findings of the three), with the CredData TEST scores of the three already known, and not by the rule of our training
stack (the best F1 on the recipe's own `select` split): its count on code-fp-b is optimistic
([details](../benchmarks/RESULTS.md#the-pre-registered-criteria-and-the-choice-of-the-released-model)).

**Pre-registered criteria.** Both CredData criteria of the recipe pass; both false-alarm guards on code-fp-b fail
(571, 360 and 455 findings per seed and 410 for the ensemble, against at most 129 and 107). The model was released
after reading a sample of such findings, most of them credential-shaped test data, and with three seeds instead of
the six our release checklist asks for
([details](../benchmarks/RESULTS.md#the-pre-registered-criteria-and-the-choice-of-the-released-model)).

## Responsible use

A secrets scanner can find other people's secrets. Scan what you own or are authorized to assess. PureByte prints
findings masked by default and never verifies credentials against any service; if you find a live credential in
someone else's project, report it to its owner privately.

## Versions

| Version | Date | Changes |
|---|---|---|
| 1.0.0 | 2026-09-24 | First public release |
