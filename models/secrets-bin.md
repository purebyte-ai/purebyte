# Model card: secrets-bin

An **AI Specialist** that finds credentials inside binary files: executables and libraries, bytecode, WebAssembly
modules, firmware images and any other file that a text scanner would skip. It reads the raw bytes, with no
disassembler, no `strings` pass and no tokenizer, and reports byte offsets.

It is one of the three example specialists of PureByte 1.0, which show what one byte-level architecture does on different
jobs; the architecture, the runtime and the training stack are described in the [README](../README.md).

| | |
|---|---|
| Version | 1.0.0 |
| Task | Credential detection as byte spans in arbitrary binary files |
| Input | Any file up to 64 MiB by default; zip (JAR, APK...), gzip and tar containers are opened and their members scanned (`--no-archives` scans them as they are); inputs under 24 bytes are not analyzed |
| Output | Findings: file, byte offsets (also in hex), the surrounding printable string with the credential masked (see [spec/OUTPUT.md](../spec/OUTPUT.md)) |
| Size | One file of 30,163,488 bytes (28.8 MiB); 54,170,048 parameters (54.2 M) in the network: the byte embedding, the blocks, the final norm, and the n-gram tables with their projection, 50.3 M of them in the tables, stored at 4 bits per value. The two heads add 34,439 (54,204,487 in all); the per-group scales of the ternary weights are derived, not counted. 12.6 M of the table values can never be read: the format gives every order's table the same number of rows (2^18), and the 65,536 byte bigrams reach only 65,536 rows of the order-2 table, so its other 196,608 rows (7.1 MB of the file, 23 %) are stored but unused; the addressable parameters number 41.6 M |
| Architecture | Byte-level state-space model with ternary weights (eight `ssm_v2` blocks of width 256), hashed n-gram memory tables (byte 2-, 3- and 4-grams, 2^18 buckets each) held in RAM, and a span-tagging head gated by a window head with max pooling ([docs/architecture.md](../docs/architecture.md)) |
| Default decision rule | One model, at the operating point stored in its file (`--bias` moves it); no ensemble |
| Post-processing | The `secrets-binary` profile: it merges spans, reports byte offsets and the surrounding printable string, masks the values, and drops the spans in the metadata of JAR manifests and signature files (digest lines such as `SHA-256-Digest: <base64>` and entry names, `Name: <path>`). By default the CLI reports only the findings that touch a printable string (16 or more ASCII or UTF-16LE characters); `--all-bytes` reports the others too |
| Runtime | `purebyte` 1.0.0 or newer, on an x86-64 or arm64 CPU (other little-endian CPUs through the portable build); no GPU, no network |
| License | [PureByte Model License](../MODEL_LICENSE.md) (the code of the runtime is Apache-2.0) |
| Files | `purebyte-secrets-bin-1.0.0.gguf`; checksum in [models.json](models.json) |
| Recipe | [specialists/secrets-bin](https://github.com/purebyte-ai/purebyte-train/blob/main/specialists/secrets-bin/README.md) in purebyte-train: the recipe and the evaluation plan, to rebuild or improve it |

## Intended use

- Checking build artifacts, containers' binaries, mobile and desktop packages and firmware before they ship.
- Auditing third-party binaries you are authorized to assess.
- Triage: pointing a human at the few byte ranges worth a look in megabytes of machine code.

## Out of scope

- **Proving that a binary is free of secrets.** Treat findings as leads and absence of findings as weak evidence.
- **Encrypted or compressed content.** Bytes inside compressed streams are not readable text; zip, gzip and tar
  containers are opened by the runtime (see [docs/cli.md](../docs/cli.md)), other compression is not.
- **Checking whether a credential is live.** It never contacts any service.
- **Text files.** Use [secrets-code](secrets-code.md), which is smaller and more precise on text.

## Evaluation

There is no public, labeled benchmark of credentials inside compiled binaries yet. These figures come from our own
exams, run on the released file with the `purebyte` CLI and its default options. **They cannot be reproduced as they
are**: part of the binaries are system files and vendor libraries that cannot be redistributed, and the probe is cut
from our internal corpus. The files that come from public packages are pinned in
[data/exams/binary-fp.yaml](https://github.com/purebyte-ai/purebyte-train/blob/main/data/exams/binary-fp.yaml) of
purebyte-train.

**False alarms.** Binaries that hold no credential, so every finding is a false alarm. `--all-bytes` also reports
the findings that touch no printable string; its column was measured on the files that had findings in an earlier
run that reported every finding (the other files had none there):

| Exam | Files | Findings | With `--all-bytes` |
|---|---|---:|---:|
| bin-fp-a: 24 compiled Python extension modules (scipy 1.18.1 and scikit-learn 1.9.1 wheels, Linux x86-64; public) | 24 (38 MiB) | 1 | 1 |
| bin-fp-b: 25 Linux system libraries and executables (Ubuntu 26.04 packages, PyTorch and NumPy wheels; public) | 25 (93 MiB) | 9, all in 4 of the 8 files that overlap the training corpus; 0 in the other 17 | the same 9 |
| bin-fp-b: 15 Windows DLLs (system, DirectX, MFC, a graphics driver; not redistributable) | 15 (80 MiB) | 12, in 3 files | the same 12 |
| bin-fp-c: 69 files in 14 formats absent from the training corpus of the first binary models (JAR, CLASS, PYC, WebAssembly, APK, firmware, Mach-O, ELF for ARM and AVR, object files, ar archives, PE for ARM64, Windows executables, Node.js addons; partly not redistributable) | 69 (109 MiB) | 4, in 2 class files inside JARs; 0 with `--no-archives` | 7; 1 with `--no-archives` |
| Held-out extension modules: 60 compiled extension modules of 18 Python packages, set apart before training (local builds) | 60 (34 MiB) | 2, in 1 file | the same 2 |

The 25 Linux files include `/usr/bin/perl`, a hard link to `perl5.40.1` (one file). bin-fp-c was measured on the
copies of two Node.js addons that desktop applications bundle; on 2026-09-25 the exam manifest was corrected to pin
the files npm publishes for the same package versions, and this model reports nothing on either version of them.

By default the CLI opens zip containers and scans their members: on bin-fp-c, 13,921 members of 6 JAR and 3 APK
files. The 4 findings are in two class files (guava's list of public suffixes, and Tomcat); `--all-bytes` adds 2 in
Scala class files and, outside the containers, 1 in a lookup table of a WebAssembly module. The manifests and
signature files of two signed JARs (`META-INF/*.MF`, `*.SF`) list a base64 digest for every class, and the model
flags many of those lines: 373 findings (371 of them on `SHA-256-Digest:` lines) before the profile had a rule for
them. The profile now drops every span in the digest lines and entry names of those files, with or without
`--all-bytes`.

**Recall on planted credentials (a synthetic probe).** 2,000 windows of 512 bytes cut from real binaries of our
corpus: 1,200 left untouched, 400 with a planted credential (`NAME=value`, the value alone among other strings,
`--flag=value`, embedded JSON) and 400 look-alike traps (hashes, UUIDs, public keys, placeholders). The probe is
synthetic and in-distribution: its credential values, names and traps come from the generator the model was trained
with, in the layouts of its binary mode, and its background windows from an earlier corpus of binaries from the same
machine folders as the training corpus. It shows that the model learned what its generator makes, not how it does on
real leaks, and the rival was not built for the generator's formats. Of the 400 credentials, 360 can be told apart
from an identifier by looking at them. Each window is scanned by the CLI as a 512-byte file, with the default options
(`--all-bytes` gives the same figures):

| | secrets-bin 1.0.0 | `strings` + regular expressions |
|---|---:|---:|
| Recall on the 360 decidable credentials | **1.000** | 0.803 |
| Precision (traps flagged count as false alarms) | 0.989 | 0.876 |
| Findings on the 1,200 untouched windows | 0 | 0 |

The remaining 40 are generic values with no name next to them, which nothing can decide from the bytes alone: the
model flags 1 of them. 360 of 360 is a recall of 1.000 with a 95 % Clopper-Pearson interval of 0.990 to 1.000; over
the recipe's six seeds, 0.989 to 1.000 (two seeds found 356 of the 360).

**The recipe's six seeds.** The recipe trains six models from different random seeds and releases the best one on its
own validation data, before any exam is read. Measured with the research code (the same weights, n-gram tables at 32
bits instead of 4), mean and maximum over the six seeds: bin-fp-a 2.7 and 6; bin-fp-b Windows DLLs 13.2 and 27;
bin-fp-b Linux files, without the files that overlap the training corpus, 3.5 and 8; bin-fp-c, idem, 13.0 and 32;
held-out extension modules 0.8 and 2; probe recall on decidable credentials, lowest seed 0.989. The released seed
scored 2, 12, 0, 0, 2 and 1.000 there. The research code reports every span and scans containers as they are, like
`--all-bytes --no-archives`, and storing the tables at 4 bits changes the spans of about 1 % of windows: that is why
the released file's `--all-bytes` counts above differ by one finding here and there.

**Pre-registered criteria.** Every false-alarm criterion of the recipe passes. The probe criterion, a recall of at
least 0.99 for every seed, fails: two of the six seeds reach 0.989 (4 of the 360 missed), and the released seed 1.000.
The model was released with that criterion failed
([details](../benchmarks/RESULTS.md#the-pre-registered-criteria-of-secrets-bin)).

## Known limitations

- **False alarms depend on the file format.** Data regions that look structured (bitmaps, lookup tables of
  WebAssembly modules or firmware, runs of a repeated byte, compressed bytes) can produce findings that are not
  credentials. Formats that are rare in the wild are the most exposed.
- **Credentials need readable bytes.** A credential split across non-contiguous bytes, encoded, encrypted or inside a
  compressed stream the runtime does not open is not found; generic values with no name next to them are mostly
  missed (they cannot be told apart from identifiers).
- **Short strings are left out by default.** A finding must touch a printable string of 16 or more characters, so a
  credential stored in a shorter string, with no longer string around it, is reported only with `--all-bytes`. The
  probe plants every credential in a longer string, so it does not measure this cost.
- **Digest lists are handled by a rule, not by the model.** The manifests and signature files of signed JARs list a
  base64 digest per class, and the model flags many of those lines (373 findings in two signed JARs of bin-fp-c
  without the rule). The profile drops every span in their digest lines and entry names, with or without
  `--all-bytes`; lists of digests in other formats have no such rule. Containers are opened by default; with
  `--no-archives` their compressed bytes are scanned as they are, which also misses any credential inside the members.
- **The optional string prefilter skips windows without printable text** (runs of at least 16 ASCII or UTF-16
  characters). It speeds up real binaries several times; what it can miss lies outside printable strings, which the
  default rule leaves out anyway (`--all-bytes` does not). It is off by default.
- **Confidence values are not calibrated yet.**
- **Throughput is modest** (about 65 KB/s on a 12-core desktop, [docs/performance.md](../docs/performance.md)): scan
  release artifacts, not whole disks.

## Training data

- **Generated examples in real binaries.** Windows of real binaries into which the generator of
  [secrets-code](secrets-code.md) injects credentials and look-alike negatives in binary layouts (between NUL bytes
  like a string table, the value alone, long hex digests as negatives). For the released weights, the binaries came
  from the Linux system folders of the training machine (`/usr/bin`, `/usr/lib`, `/opt` and the like); the data
  scripts rebuild a comparable corpus from public packages pinned by version and SHA-256, so a retraining lands close,
  not bit for bit.
- **Real negatives.** 7 windows of every training step (45 % of the nominal batch of 16, added to it: 7 of the 23
  windows of a step) come from 78,283 windows of binaries without credentials: Windows executables and libraries of
  the system folders, files of twelve other formats (JAR, APK, PYC, WebAssembly, Mach-O, Node addons, firmware, ar
  archives, object files, ELF shared libraries, and Windows executables and DLLs from outside the system folders) and
  compiled extension modules of 63 Python packages. None of them is an exam file; many are system files that cannot
  be redistributed, so the builder of this stream is not in purebyte-train yet.
- **Overlap.** Parts of 10 exam files (8 of the 25 Linux files of bin-fp-b and 2 files of bin-fp-c) are also in the
  binary corpus the generated examples were cut from; the six-seed figures above leave them out.

The recipe and the scripts that rebuild the corpus from public sources are in the training repository,
[purebyte-train](https://github.com/purebyte-ai/purebyte-train):
[specialists/secrets-bin](https://github.com/purebyte-ai/purebyte-train/blob/main/specialists/secrets-bin/README.md) and
[data/](https://github.com/purebyte-ai/purebyte-train/blob/main/data/README.md).

## Responsible use

Scan binaries you own or are authorized to assess. Findings are masked by default and credentials are never verified
against any service. If you find a live credential in someone else's software, report it to its vendor privately.

## Versions

| Version | Date | Changes |
|---|---|---|
| 1.0.0 | 2026-09-24 | First public release |
