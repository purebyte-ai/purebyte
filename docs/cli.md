# Command-line reference

One program, `purebyte`, with one command per job. This page documents version 1.0.0; `purebyte <command> --help`
prints the options of the version you have.

| Command | What it does |
|---|---|
| [`scan`](#scan) | Scan files, directories, standard input, git changes or archives, and report findings |
| [`decide`](#decide) | Answer for one small input (up to 4,096 bytes): the decision and the spans |
| [`redact`](#redact) | Write a copy of the input with every detected span replaced by a typed marker |
| [`serve`](#serve) | Run the local HTTP API |
| [`models`](#models) | List, download and verify AI Specialists |
| [`bench`](#bench) | Measure latency and throughput on this machine |
| [`info`](#info) | Describe the runtime, or what a model file declares |
| [`version`](#version) | Print the version |

## Conventions

- Options are written `--name value` or `--name=value`. `-h` is `--help`, and `--` ends the options (everything after
  it is a path, even when it starts with `-`). A lone `-` means standard input.
- An unknown option, a missing value or a non-repeatable option given twice is an error; nothing is silently ignored.
  Numbers are checked (`--threads` takes an integer from 1 to 1024, `--bias` any finite number, and so on).
- Reports go to standard output; progress, summaries and errors go to standard error.

**Exit status**

| Status | `scan` | `decide` | Other commands |
|---|---|---|---|
| 0 | No finding of severity `error` (nothing found, or only warnings) | Negative | Success |
| 1 | At least one `error` (with `--strict`: at least one finding) | Positive | |
| 2 | Something could not be scanned (the report says what and why), or the command failed | Error | Error |

A scan that has both findings and inputs it could not scan exits with 2. Findings in test, example and documentation
paths are warnings with the `secrets-code` profile: see [Severity](#severity).

**Environment**

| Variable | Effect |
|---|---|
| `PUREBYTE_HOME` | The models directory (see [models](#models)) |
| `PUREBYTE_CATALOG` | A catalog file to use instead of the one built into the binary: same schema as [models/models.json](../models/models.json), for example with `file://` URLs of a local mirror. Each `file` must be a plain file name ending in `.gguf` (letters, digits, `.`, `_` and `-`): a catalog that gives a path there is refused |
| `PUREBYTE_SERVE_TOKEN` | The token of [serve](#serve) when neither `--token` nor `--token-file` is given. Set but empty, it is refused |
| `PUREBYTE_TEST_FAIL_INPUT` | For the test suite only: makes the analysis of the input of that name fail, to test how `scan` retries a batch. It can only turn a result into a failure (exit status 2) |

**External programs.** `scan --staged` and `scan --git-diff` run `git`; `models pull` downloads with `curl` (HTTPS or
`file://` only). Nothing else is started, and no other command connects to the network (`serve` only listens, on
loopback by default). Both programs are looked up in the
absolute folders of `PATH` only: the current folder and the empty, `.` or relative entries of `PATH` are never
searched, so a `git` or `curl` that a scanned repository ships at its root is never run (on Windows, only `.exe` and
`.com` files are considered). A program that is not found is an error that says so.

## Choosing a model

`scan`, `decide`, `redact`, `serve` and `bench` share these options:

| Option | Effect |
|---|---|
| `--model NAME` | An installed specialist (`secrets-code`, or `secrets-code@1.0.0` for one version; `secrets-code-ensemble` for its optional ensemble of three) or the path of a model file: a name that ends in `.gguf`, starts with `.` or has a folder in it (`./my-model`). Default: `secrets-code` (`redact` has no default) |
| `--ensemble FILE` | Add a model file to the ensemble. Repeatable |
| `--no-ensemble` | Use only the first model of an ensemble: faster, and it finds less |
| `--profile NAME` | The post-processing profile: `none` (the heads' outputs as they are), `secrets-code`, `secrets-binary` or `redact` |
| `--bias X` | The operating point of span heads: lower is more precise, higher finds more. Default: the value stored in the model |
| `--type-bias T=X,...` | The operating point per entity type, for example `EMAIL=1,PHONE=-0.5`; types not listed keep theirs |
| `--votes K` | How many ensemble members must agree on a finding. Default: a majority |
| `--min-confidence X` | Drop spans below this confidence |
| `--threads N` | Worker threads. Default: the number of hardware threads (logical CPUs) minus one, at least one |
| `--intra-threads N` | Threads that share the work of one window: `0` automatic (idle threads join a window when a batch has fewer windows than threads, up to 8 per window; the default), `1` never split a window |
| `--kernel NAME` | `auto` (default), `scalar`, `avx2` or `neon`. Every kernel gives bit-identical results; forcing one is for testing |

A name is resolved through the catalog, never as a file of the current folder (the integrations run from the root of
the repository they scan, which must not be able to replace `secrets-code` by shipping a file of that name): the
specialist's model file and its ensemble members are loaded from the models directory, and each file is checked
against its published SHA-256 when the catalog has one; the bytes checked are the bytes loaded. Without a version,
the highest version that this `purebyte` can run is chosen (a catalog entry can require a newer one with
`min_runtime`). The profile is the one given with `--profile`, else the one the model file declares, else the one the
catalog lists, else `none`.

## scan

```text
purebyte scan [options] [PATH ...]
purebyte scan [options] --staged [REPOSITORY]
purebyte scan [options] --git-diff REF [REPOSITORY]
```

**What to scan**

| Input | How |
|---|---|
| Files and directories | `purebyte scan src/ config/app.yaml`. Directories are walked recursively; links met on the way (symbolic links, and junctions on Windows) are not followed |
| Standard input | `purebyte scan -` (reported as `<stdin>`) |
| Staged changes | `purebyte scan --staged`: the staged version of every added, copied, modified or renamed file, and of every file whose type changed (a link that became a file); findings on added lines only |
| A branch or a pull request | `purebyte scan --git-diff origin/main`: the working-tree version of every file changed since that reference; findings on added lines only |
| Archives | The members of zip files (and jar, apk, whl...), gzip and tar files, recursively; members are reported as `outer.jar!/inner/path` |

With `--staged` and `--git-diff`, the optional path is the repository (default: the current directory); whole files
are scanned, so a finding keeps its full context, and findings on lines the change did not add are dropped. File
names are always taken literally, never as git patterns or revisions (`[ab].py`, `0:app.py` and `:(glob)x.env` are
those files), and `--staged` reads each file from the index by its object id. `--git-diff` reads the working tree as
a walk does: a changed file that is a link there is not followed, since it may point anywhere on the machine. A file
that is only renamed or moved adds no line, so it has no finding; submodules are other repositories and are not
entered. The added lines are those of the file scanned: git's textconv filters and external diff programs are not
run, and settings of the repository such as `diff.relative` do not change what is compared.

**Which files.** The model's profile decides what a directory walk reads:

| Profile | Files read while walking | Folders skipped | Largest input |
|---|---|---|---|
| `secrets-code` | Source code, configuration and text files, by extension or name (`.py`, `.js`, `.yaml`, `.env`, `.tf`, `.pem`, `Dockerfile`, `.npmrc`, ...) | `.git`, `node_modules`, `vendor`, `dist`, `build`, `target`, `.next`, `__pycache__`, `.venv`, `venv` | 4,000,000 bytes |
| every other profile | Every file | `.git` | 64 MiB |

Files named on the command line are always read, whatever their name. Empty files are skipped. An input over the
largest size is reported as not scanned, with the reason (its size and the limit, in bytes), without being read.
With `--staged` and `--git-diff`, changed files are selected by the same name and size rules.

**Archives** are expanded in memory within fixed limits against archive bombs: 3 levels of nesting, 64 MiB per
decompressed member, 256 MiB decompressed per archive (members refused on the way count too) and a compression ratio
of 200 per member. Members that break a limit, and zip entries that share their data with another entry, are reported
as not scanned; at most 1000 such failures are listed per archive, and a last one counts the others. A member's name
inside its archive is cut in the middle beyond 4 KiB.

**Options**

| Option | Effect |
|---|---|
| `--format json` | One JSON document (the default) |
| `--format jsonl` | One JSON line per input, written as each input finishes |
| `--format sarif` | SARIF 2.1.0, for code scanning and security dashboards |
| `--out FILE` | Write the report to a file instead of standard output |
| `--reveal` | Also print the detected values in clear. Use with care: the report then holds secrets |
| `--staged` | Scan the staged changes (see above) |
| `--git-diff REF` | Scan the changes since `REF` (see above) |
| `--prefilter` | Skip windows that no printable string touches (a run of 16 or more ASCII or UTF-16 characters). Several times faster on binaries; not exact: it can miss what lies outside such strings |
| `--all-bytes` | `secrets-binary` profile: also report findings that touch no printable string (a run of 16 or more ASCII or UTF-16LE characters). By default only the findings that touch one are reported |
| `--early-exit` | Stop clearly negative windows after the first layers, for models that ship an exit head. Not exact by construction |
| `--per-model` | Also report the findings of every ensemble member (`per_model` in each result) |
| `--strict` | Exit 1 on warnings too, not only on errors (see [Severity](#severity)) |
| `--tests no` | Skip test, example and documentation paths while walking folders and git changes, and leave out the warnings that remain (archive members, files named explicitly). `--tests yes` is the default |
| `--keep-examples` | `secrets-code` profile: also report well-known documentation example keys, which are ignored by default |
| `--exclude GLOB` | Leave out the paths that match GLOB, as a `.purebyteignore` line (repeatable): see [Ignoring findings](#ignoring-findings) |
| `--no-archives` | Scan zip, gzip and tar files as they are instead of their members |

Plus the [model options](#choosing-a-model).

**Output.** The JSON document has the runtime version, the model (`name`, `version`, `profile`, and the `files` used
with their SHA-256), the ensemble (`members`, `votes_needed`), the runtime (`kernel`, `threads`), the `scope` of a git
scan, one result per scanned input, the flat list of `findings`, the `failures` (`file` and `reason` of every input
that could not be scanned) and `stats` (with `by_severity`: `{"error": N, "warning": N}`). A finding has `file`,
`kind`, `severity`, `start` and `end` (byte offsets, end exclusive), `line` and `col` for text, `confidence`, `votes`
and a masked `snippet_masked`; the full list of fields, the JSON Lines layout and the SARIF mapping are specified in
[spec/OUTPUT.md](../spec/OUTPUT.md).

If the runtime fails on a batch of inputs, `scan` analyzes those inputs again one by one: an input that fails on its
own is reported as not scanned (with the runtime's message as the reason, so the exit status is 2), and every other
input is reported as usual.

When it finishes, `scan` prints a summary on standard error (the second line only when there are warnings and no
`--strict`):

```text
purebyte: 4 file(s), 1.2 KiB, 5 finding(s) (1 error(s), 4 warning(s)), 0.1 s, 0 not scanned
purebyte: warnings (test, example or documentation paths) do not change the exit status; --strict counts them
```

**Paths.** Every path the tool writes (file names in reports, model files, the models directory, messages) uses `/`
as the separator on every platform: `purebyte scan src\app` on Windows reports `src/app/settings.py`. Paths keep the
form you gave them otherwise: relative stays relative, absolute stays absolute.

**Writes.** A report, a redacted copy or a map that cannot be written completely (a full disk, a closed pipe) is an
error with exit status 2, never a partial result.

### Severity

Every finding has a `severity`: `error`, or `warning` when the profile lowers it by the path of its file. With the
`secrets-code` profile, findings in test, example and documentation paths are warnings: most of what a secrets model
finds in a real repository sits there, and it is mostly credentials made for tests and examples. Warnings are
reported in every format (in SARIF as `level: warning`, so code scanning shows them without failing a check on them)
but do not change the exit status: only errors make `scan` exit 1. `--strict` makes warnings count too; `--tests no`
skips those paths altogether.

A path is a test, example or documentation path when a folder in it is named `test`, `tests`, `spec`, `specs`,
`__tests__`, `testdata`, `test-data`, `fixture(s)`, `jvmTest`, `androidTest`, `example(s)`, `sample(s)`, `doc(s)`,
`mock(s)`, `__mocks__`, `e2e` or `cypress` (any case), when the file is named as a test (`app.test.js`,
`user_spec.rb`, `handler_test.go`, `test_api.py`, `KeyStoreTest.java`, `LoginTests.cs`), or when it is a document
(`.md`, `.rst`, `.adoc`, `.markdown`, `.mdx`, `.asciidoc`). The exact rule is in
[spec/OUTPUT.md](../spec/OUTPUT.md#51-severity).

Only the path **inside the project** counts, never the folders above it:

| You run | The rule looks at |
|---|---|
| `purebyte scan .` or `purebyte scan src tests` (relative paths) | the path as written: `./tests/app_test.go`, `src/app.py` |
| `purebyte scan /home/me/test/repo` (an absolute path, or one starting with `..`) | the path below the folder named: `src/app.py` is an error even though `/home/me/test/` holds a `test` |
| `purebyte scan /home/me/repo/tests/app.cfg` (a file named that way) | its file name: `app.cfg` |
| `purebyte scan --staged`, `--git-diff REF` | the path in the repository: `tests/app.cfg` |

The `secrets-binary` profile (and `redact` and `none`) reports every finding as an error: a credential compiled into
a binary ships with it, wherever the binary sits in the tree.

### Ignoring findings

Two ways to tell `scan` that something is fine, the same ones other secret scanners use:

**On the line.** With the `secrets-code` profile, a finding that starts on a line carrying `purebyte:allow` is not
reported. Put it in a comment next to the value you accept:

```python
TEST_TOKEN = "3q2+7wAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="  # purebyte:allow (a fixture, not a credential)
```

**By path.** `--exclude GLOB` (repeatable) and the lines of a `.purebyteignore` file leave paths out of a scan:

```text
# A folder anywhere (a trailing / matches folders only)
generated/
# A file name anywhere
*.min.js
# A leading / (or any /) anchors the pattern at the top of the scan
/vendor
# ** stands for any number of folders
docs/examples/**
```

The patterns follow `.gitignore`: a pattern without `/` matches a name at any depth, one with `/` matches the path
from the top of the scan, `*` and `?` never cross a `/`, and a folder that matches leaves out everything below it.
Lines that start with `#` and empty lines are ignored (a `#` later in a line is part of the pattern); negation (`!`) and character classes (`[abc]`) are refused
rather than guessed. `.purebyteignore` is read at the top of each folder named on the command line, and at the root
of the repository with `--staged` and `--git-diff`; `--exclude` patterns apply to the same paths. A file named
explicitly on the command line is always scanned. The file comes with the repository scanned, so a `.purebyteignore`
that is a symbolic link is refused rather than followed, and matching stays fast whatever the patterns hold (`**`
repeated any number of times included).

In CI, alerts uploaded as SARIF can also be dismissed in GitHub's code scanning view, where they stay dismissed.

## decide

```text
purebyte decide [options] [FILE | -]
purebyte decide [options] --text TEXT
```

For one small input of up to 4,096 bytes: one line, one event, one request, one snippet. Without `FILE` or `--text`,
it reads standard input. It prints one JSON object, `{"decision": "positive" | "negative", "model", "result",
"findings"}`, where `result` is the generic per-input result and `findings` the flat list. The decision is positive
when the profile reports a finding or, for a model without span heads, when its first `choice` head decides a class
other than 0.

| Option | Effect |
|---|---|
| `--text TEXT` | Decide on this text instead of a file |
| `--reveal` | Also print the detected values in clear |
| `--all-bytes` | `secrets-binary` profile: also report findings that touch no printable string, as `scan --all-bytes` |
| `--query FIELD` | A field to extract, for query-conditioned models. Repeatable |

Plus the [model options](#choosing-a-model). Archives are not expanded. Exit status: 0 negative, 1 positive, 2 error.

## redact

```text
purebyte redact --model NAME [options] [INPUT | -]
purebyte redact --spans SPANS.json [options] [INPUT | -]
purebyte redact --restore REDACTED --map MAP.json [--out FILE]
```

Writes the input byte for byte, except that every detected span becomes a typed marker such as `[EMAIL_1]`; the same
value always gets the same marker. Nothing outside the spans changes, and that is checked before anything is written.
Any model with a span head can redact: with `secrets-code`, credentials become `[secret_1]`, `[secret_2]`, and so on
(the marker takes the model's name for the type). Inputs of any length are analyzed. With `secrets-code`, which reads a
UTF-16 file as UTF-8, the copy stays UTF-16 (its byte-order mark and the markers included), and the report and the map
say so in `"encoding"`.

| Option | Effect |
|---|---|
| `--types T1,T2` | Redact only these entity types |
| `--out FILE` | Write the redacted copy to a file (default: standard output) |
| `--report FILE` | Write the report: types, offsets and markers, never the values |
| `--map FILE` | Write the reversible map. It holds the original values: treat it as a secret. On Linux and macOS it is made readable by its owner only, also when the file already existed, and a symbolic link in its place is refused rather than followed; on Windows it gets the permissions of its folder |
| `--spans FILE` | Redact the spans in this JSON file instead of running a model: `[[start, end, "TYPE", confidence, "source"], ...]` (the last two are optional) or `[{"start": ..., "end": ..., "type": ...}, ...]` |
| `--restore FILE` | Rebuild the original of a redacted file, with `--map`. A redacted file edited after redaction is refused |

Plus the [model options](#choosing-a-model) (`--model` is required unless `--spans` or `--restore` is given, and the
model options are refused with them; `--restore` takes only `--map` and `--out`). The report and the map are
specified in [spec/OUTPUT.md](../spec/OUTPUT.md#9-redaction-output-report-and-map). Exit status:
0 success, 2 error.

## serve

```text
purebyte serve [--model NAME ...] [options]
```

Loads the models once and answers HTTP requests until it is stopped. Endpoints, parameters, errors and security:
[api.md](api.md#http-api).

| Option | Effect |
|---|---|
| `--model NAME` | A model to serve. Repeatable; the first one answers requests that name none. Default: `secrets-code` |
| `--host ADDRESS` | The address to listen on (default `127.0.0.1`). Anything else prints a warning: set a token |
| `--port N` | The port (default `8421`) |
| `--token TOKEN` | Require `Authorization: Bearer TOKEN` on every endpoint except `/health`. Other users of the machine can read a command line: prefer `--token-file` or `PUREBYTE_SERVE_TOKEN` (the server warns) |
| `--token-file FILE` | The same, with the token on the first line of `FILE` (spaces and the line end are trimmed). Without `--token` and `--token-file`, the token is `PUREBYTE_SERVE_TOKEN` when it is set. A token given empty, by any of the three, is refused: the server never starts without one by mistake |
| `--allow-host NAME` | Also answer requests addressed to `NAME` (repeatable), besides `127.0.0.1`, `localhost`, `[::1]` and the address given with `--host`. The `Host` check applies on loopback, and on any address when there is no token ([api.md](api.md#security)) |
| `--archives` | Scan the members of zip, gzip and tar inputs. Off by default: inference runs one request at a time and cannot be interrupted, and an archive expands to far more than its own size (within the [archive limits](#scan)) |
| `--http-threads N` | Threads that handle connections (default 4). Inference itself runs one request at a time on `--threads` |
| `--max-body-mb N` | The largest request body, in MiB (default 80) |
| `--quiet` | No access log on standard error |

The [model options](#choosing-a-model) apply to every served model; `--ensemble` is only accepted with a single
`--model`.

## models

```text
purebyte models list [--json]
purebyte models pull NAME[@VERSION] [--force]
purebyte models verify [NAME]
```

| Action | Effect |
|---|---|
| `list` | The catalog and the state of each specialist: `installed`, `not installed`, `installed (no checksum to verify)`, `CORRUPT (checksum mismatch)`, or `needs purebyte X.Y.Z or newer` when its `min_runtime` is newer than this `purebyte`. `--json` for a machine-readable list |
| `pull` | Download every file of a specialist (the highest version this `purebyte` can run, or the one given), check its size and SHA-256 against the catalog, and install it. Files already installed and verified are kept unless `--force` is given. A specialist that is not published yet, or that needs a newer `purebyte`, is refused before anything is downloaded |
| `verify` | Recompute the SHA-256 of every installed file against the catalog and the `.sha256` file next to it, and of any other `.gguf` file of the directory that has a `.sha256` file. With a name, check that specialist's files (the version `pull` would install, or the one given): a name the catalog does not know, or a file of it that is missing, is an error too. Exit status 2 on any problem |

The models directory is `PUREBYTE_HOME` when set, else a per-user directory:

| System | Directory |
|---|---|
| Linux | `$XDG_DATA_HOME/purebyte/models`, or `~/.local/share/purebyte/models` |
| macOS | `~/Library/Application Support/purebyte/models` |
| Windows | `%LOCALAPPDATA%\purebyte\models` |

Where none of these variables is set (some service managers and cron jobs set no `HOME`), the commands that need
the directory stop and ask for `PUREBYTE_HOME`; `purebyte info` shows `null`. The directory is never a folder
relative to the current one, which is often the repository being scanned.

Each file is stored under its catalog name with a `<file>.sha256` next to it, in the format of `sha256sum`. Offline
installation: [install.md](install.md#models).

## bench

```text
purebyte bench [--model NAME] [options]
```

Measures, on this machine, how long the main model takes on one input of each size, in its windows (the median and
the 95th percentile of several runs, after a warm-up), and its throughput on a longer input, and prints the kernel and
the threads it used. It runs the model only: no post-processing, no ensemble. Inputs are generated text that looks
like source code; the time of a window does not depend on its content.

| Option | Effect |
|---|---|
| `--sizes B1,B2,...` | Input sizes of the latency test, in bytes (default `64,256,1024,4096`) |
| `--repeats N` | Measurements per size (default 30) |
| `--throughput-kb N` | Size of the throughput test, in KB (default 256) |
| `--json` | Machine-readable output |

Plus the [model options](#choosing-a-model). When you share numbers, include the CPU, the operating system and
whether the machine was idle ([performance.md](performance.md#measure-it-yourself)).

## info

```text
purebyte info            # the runtime: versions, CPU kernel, cores, default threads, models directory
purebyte info MODEL      # what a model declares: window, blocks, heads, labels, operating point, profile
```

Both print JSON. `MODEL` is an installed specialist or a model file; for a specialist, every file of its ensemble is
described with its size and SHA-256.

## version

```text
purebyte version         # prints "purebyte 1.0.0"
```
