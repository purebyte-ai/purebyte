# PureByte output format

This document is normative for the results of the runtime: what the C API (`pb_detect`, `pb_redact`), the
`purebyte` command-line tool and the local HTTP server return. How a model computes its raw outputs per window is in
[FORMAT.md](FORMAT.md); this document starts from those window outputs.

- [1. Conventions](#1-conventions)
- [2. The document result](#2-the-document-result)
- [3. Per-input results](#3-per-input-results)
- [4. Aggregation over windows](#4-aggregation-over-windows)
- [5. Findings](#5-findings)
- [6. Masking](#6-masking)
- [7. Command-line reports: JSON, JSON Lines](#7-command-line-reports-json-json-lines)
- [8. SARIF 2.1.0](#8-sarif-210)
- [9. Redaction: output, report and map](#9-redaction-output-report-and-map)
- [10. Compatibility](#10-compatibility)

## 1. Conventions

- Every result is one UTF-8 JSON value. Invalid UTF-8 in a name or a revealed value is replaced by U+FFFD.
- Offsets are byte offsets in the input, 0-based; a span is half-open, `[start, end)`. Lines and columns are 1-based.
  `col` counts bytes from the start of the line; SARIF columns count Unicode code points (section 8).
- File names (`file`) are the inputs' names as the caller gave them. The command-line tool and the Python bindings
  write every path with `/` as the separator on every platform, Windows included (`src/app/settings.py`,
  `C:/work/app.py`), and keep relative paths relative, so reports compare equal across operating systems.
- Probabilities and confidences are in [0, 1]. They are the model's own probabilities, not calibrated across models.
  Floating-point numbers are written with the fewest digits that read back as the same value.
- **Detected values never appear in clear** unless the caller asks for them (`PB_DETECT_REVEAL`, `--reveal`,
  `reveal=yes`). Without that request every view of a value is masked (section 6); with it, the masked fields stay
  and the clear ones are added next to them.
- Fields may be added in later versions; readers MUST ignore fields they do not know (section 10).

## 2. The document result

`pb_detect` runs a detector (one model or an ensemble, plus a post-processing **profile**) over a list of inputs and
returns one object:

| Field | Type | Meaning |
|---|---|---|
| `profile` | string | the profile that produced the results: `none`, `secrets-code`, `secrets-binary` or `redact` |
| `models` | integer | ensemble members that ran (1 without an ensemble) |
| `votes_needed` | integer | members that must agree on a span for it to be reported |
| `results` | array | one object per analyzed document, in input order (section 3): an input, or each wanted member of an archive input |
| `findings` | array | the flat list of findings of every input, in input order (section 5) |
| `failures` | array | `{file, reason}` for every input or archive member that could not be analyzed, and why (it has no result) |

The profile is chosen by the caller, else declared by the model file (`purebyte.profile`), else `none`. `none`
reports the heads' outputs as they are; `secrets-code` and `secrets-binary` add rules of their own (formats,
documentation examples, strings in binaries) before the findings are listed, and `secrets-code` lowers the severity
of findings in test, example and documentation paths (section 5.1); `redact` produces redacted copies (section 9).
Inputs inside archives (zip, jar, apk, gzip, tar) are named `outer.zip!/inner/path`.

## 3. Per-input results

| Field | Type | Present | Meaning |
|---|---|---|---|
| `file` | string | always | the input's name |
| `input` | integer | always | its index in the call |
| `bytes` | integer | always | its size |
| `windows` | integer | always | windows (or stream chunks) of the input, those the prefilter skipped and those the exit head stopped included (they are counted apart below) |
| `windows_skipped` | integer | when not 0 | windows the optional prefilter skipped |
| `windows_exited` | integer | when not 0 | windows the exit head stopped (optional early exit) |
| `positive_windows` | integer | always | windows whose first `choice` head decided a class other than 0 |
| `decisions` | object | always | per `choice` head, by head name: `{index, label?, probability, probabilities, aggregate}` |
| `labels` | object | always | per `multilabel` head: the labels that are on, `[{index, label?, probability}]` |
| `scores` | object | always | per `score` head `{values, names?}`; per `ordinal` head `{level, label?, values}` |
| `spans` | array | always | the reported spans: `{type, start, end, confidence, votes}` |
| `fields` | array | query-conditioned models | `{name, start, end, confidence, value_masked, value?}` |
| `byte_map` | object | models with `byte_map` heads | per head: regions `[{start, end, index, label?}]` covering the bytes of the windows where the head was computed (every byte of the input, unless windows were skipped, stopped or gated, or the input is shorter than the shortest window) |
| `per_model` | array | on request | every ensemble member's own findings, in the format of section 5 |
| `counters` | object | when a profile sets one | profile counters, e.g. `utf16_converted`, `ignored_documentation_examples` |
| `note` | string | when relevant | e.g. "the input is shorter than the model's shortest window: not analyzed" |

`label` fields hold the name the model gives the class, label, level or entity type
(`purebyte.head.<i>.labels`) and are present only when the model names it. A span's `type` is the entity type's name
(`entity_<k>` when unnamed). `votes` is the number of ensemble members that found the span.

`fields` holds, for a query-conditioned model, one entry per span: `name` is the query that appears last on the
span's line before it (empty when none does); `value_masked` is the masked value (section 6).

`byte_map` regions are maximal runs of bytes with the same label; each byte takes the label of the earliest window
that covers it (the one with the most left context).

## 4. Aggregation over windows

A window-level head gives one output per window; `purebyte.head.<i>.aggregate` (FORMAT.md, section 11) says how the
windows of one input combine. Only windows where the head was computed take part (not the skipped, stopped or gated
ones); a head computed in no window is absent from the result.

| Head | `max` | `mean` | `vote` | `any` |
|---|---|---|---|---|
| `choice` | the probabilities and class of the window with the highest `1 - p[0]` | the mean of the probabilities (in double precision); the class is their argmax | each class's share of the windows' decisions; the class with the most windows | as `max` |
| `multilabel` | per label, the highest probability | per label, the mean probability | — | as `max` |
| `score` | per output, the highest value | per output, the mean value | — | — |
| `ordinal` | the window with the highest level | the mean of the cumulative probabilities; the level counts those above 0.5 | — | — |

`score` heads also accept `min` (per output, the lowest value). A `multilabel` label is on when the aggregated
probability reaches its threshold. Ties go to the earliest window or the lowest index. A value that is not a finite
number (only a computation that overflows produces one; FORMAT.md, section 11.2) is never positive and never the
highest: for `max` and `any`, a `choice` head whose windows all have NaN probabilities reports the first of them, and
JSON writes each such value as `null`.

The `tag` head (a model has at most one) is not aggregated: the spans of every window are collected in input coordinates and the profile combines
them. The `none` and `redact` profiles use the typed resolution: overlapping spans of one type become their union
with the higher confidence; where spans of different types overlap, the best one owns those bytes (a rule-validated
type first, then the higher confidence, the longer span, the type the model lists first) and the others keep the rest,
so the reported spans cover exactly the union of the window spans. Spans below the caller's minimum confidence are
dropped, and with an ensemble a span is kept when `votes_needed` members found it. The `secrets-code` and
`secrets-binary` profiles add their own rules on top (formats, documentation examples, strings; for binaries, the
metadata lines of JAR manifests and signature files, digests and entry paths, are never reported). With
`secrets-code`, a finding that starts on a line carrying the marker `purebyte:allow` is not reported.

## 5. Findings

`findings` is the flat list of what the profile reports, one object per span, for tools that only want a list:

| Field | Type | Present | Meaning |
|---|---|---|---|
| `file` | string | always | input name |
| `kind` | string | always | entity type name |
| `severity` | string | always | `error` or `warning` (section 5.1) |
| `start`, `end` | integer | always | byte span in the input |
| `line`, `col` | integer | text inputs | 1-based line and byte column of `start` |
| `end_line` | integer | text inputs | `line` plus the number of line feeds in the span, so the line after the last byte when the span ends with a line feed (absent inside base64) |
| `offset_hex` | string | binary inputs | `start` in hexadecimal, `0x…` |
| `confidence` | number | always | the span's confidence |
| `votes` | integer | always | ensemble members that found it |
| `inside_base64` | boolean | when true | the span was found in the decoded text of a base64 run |
| `inner_start`, `inner_end` | integer | inside base64 | the span in the decoded bytes; `start`/`end` are then the whole run |
| `snippet_masked` | string | always | the value, masked |
| `context_masked` | string | text inputs | the line, with this and every other finding on it masked; on a very long line, the part of it around the finding (§6) |
| `string_masked` | string | binary inputs | the printable string around it, with every finding in it masked |
| `snippet`, `context`, `string` | string | on request | the clear views (reveal) |

### 5.1 Severity

A finding is an `error` (it should fail a check) unless the profile lowers it to a `warning` by the path of its
file. Only `secrets-code` does, for test, example and documentation paths: in real repositories most of its findings
fall there, and they are mostly credentials made for tests and examples (which leak datasets such as CredData still
count as secrets). Warnings are reported; they fail a scan only when asked (`--strict`, section 7).

A path is a test, example or documentation path when (in any case, with `/` or `\` as separators):

- a folder (or the file) is named `test`, `tests`, `spec`, `specs`, `__tests__`, `testdata`, `test-data`, `fixture`,
  `fixtures`, `jvmTest`, `androidTest`, `example`, `examples`, `sample`, `samples`, `doc`, `docs`, `mock`, `mocks`,
  `__mocks__`, `e2e` or `cypress`, anywhere in the path (`src/test/java/…`, `web/__tests__/…`);
- the file is named as a test: `.`, `_` or `-`, then `test`, `tests`, `spec` or `specs` right before a single
  extension of letters and digits (`app.test.js`, `app.spec.ts`, `user_spec.rb`, `handler_test.go`); `test_*.py`; or
  a `.java`, `.kt`, `.scala`, `.swift` or `.cs` file whose name ends in `Test` or `Tests` with a capital T
  (`KeyStoreTest.java`, `LoginTests.cs`; `Latest.java` is not a test), or is `test` or `tests`;
- the file is a document: `.md`, `.markdown`, `.mdx`, `.rst`, `.adoc` or `.asciidoc`.

The rule looks at the file's path **inside the project scanned**, never at the folders above it, so that a project
kept in `/home/me/test/` is not all warnings. `pb_detect_options.paths` gives that path apart from the display name
(default: the name); an archive member's is its archive's, then `!/` and its name inside the archive. The
command-line tool and the Python `scan` pass, for a relative argument, the path as written (`purebyte scan .` from the
repository root); for an absolute argument or one that starts with `..`, the part below the folder named (for a file
named that way, its file name); for `--staged` and `--git-diff`, the path in the repository. The HTTP server uses the
`filename` parameter.

`secrets-binary`, `redact` and `none` report every finding as an `error`: a binary is what gets built and shipped, and a
credential compiled into it ships with it wherever it sits in the tree; a redacted copy must hide every value,
whatever its path, and redaction passes or fails nothing.

## 6. Masking

Every output path (JSON, JSON Lines, SARIF, the HTTP server, error messages) shows a detected value through one
masking function:

- only the first line of the value is shown; a value that holds line feeds gets ` … (N lines)` appended, N being one
  more than its line feeds;
- 16 or more code points: the first 4 and the last 2 are kept; 8 to 15: the first 2 are kept; fewer: none; the hidden
  part is written as asterisks, at most 12;
- a PEM header line of a key is shown as it is: `-----BEGIN ` and `-----` around words that name a key type (`RSA`,
  `DSA`, `EC`, `ECDSA`, `ED25519`, `ED448`, `X25519`, `X448`, `OPENSSH`, `SSH2`, `PGP`, `ENCRYPTED`, `PRIVATE`,
  `PUBLIC`) and end in `KEY` or `KEY BLOCK`. It names the key type, not the key; any other label is masked;
- context lines and strings are cut at 160 code points, and a finding inside a base64 run is shown as
  `[base64 blob, N B]`;
- a view hides every span a model found inside it, not only the findings listed: the spans a profile rule dropped or
  folded into an earlier finding of the same string, and every ensemble member's findings, with or without their
  votes and whether or not `per_model` is asked for. Masking follows the spans, not the text: the same text at a place
  where no model found anything is shown as it is;
- the context of a finding is its whole line, unless the line is longer than that: then it is the part of the line
  from 256 bytes before the finding to 256 bytes after it (counting at most 4,096 bytes of the finding itself), cut at
  character boundaries. This bounds the work per finding on minified or crafted one-line files;
- findings that overlap are masked as one span, so no part of either value is ever shown in clear;
- in context lines and strings, a finding that covers only part of a token (a run of letters, digits and
  `+ / = _ - . ~`, such as a JWT or a base64 key) is masked together with the whole token, so that the rest of the
  token is not shown next to the mask. The finding's own offsets and `snippet_masked` are unchanged.

## 7. Command-line reports: JSON, JSON Lines

`purebyte scan --format json` (the default) writes one object:

| Field | Meaning |
|---|---|
| `purebyte` | runtime version |
| `model` | `{name, version, profile, files}`; `files` = the model files used, `[{path, sha256}]` |
| `ensemble` | `{members, votes_needed}` |
| `runtime` | `{kernel, threads}` |
| `scope` | `{staged: true}` or `{git_diff: REF}` when scanning changes (findings on added lines only) |
| `results` | per-input results (section 3) |
| `findings` | findings (section 5) |
| `failures` | `[{file, reason}]`: every input or archive member that could not be analyzed, and why (section 2), plus the paths that could not be read or listed |
| `stats` | `{files, bytes, seconds, findings, by_severity, files_with_findings, not_scanned}`; `by_severity` = `{error, warning}`, the findings of each severity (section 5.1); `not_scanned` = the number of `failures` |

`--format jsonl` writes one line per input: its result object (section 3) with its `findings` inside, and one line
`{file, not_scanned}` per input that could not be scanned. Lines are written as inputs finish, so a consumer can
stream them; each finding carries its `severity`, and the counts are in the summary line the tool writes to standard
error (`purebyte: N file(s), … F finding(s) (E error(s), W warning(s)), …`), for every format.

Exit status of `scan`: 0 when no finding is an `error` (nothing found, or warnings only), 1 when at least one is (with
`--strict`: when there is any finding), 2 when something could not be analyzed or the command failed (2 wins over 1).
`--tests=no` skips test, example and documentation paths while walking folders and git changes, and leaves out the
warnings that remain (archive members, files named explicitly). Exit status of `decide`: 0 when the decision is
negative, 1 when it is positive, 2 on failure.

## 8. SARIF 2.1.0

`--format sarif` (and `?format=sarif` on `POST /v1/scan`) maps findings to one SARIF run:

| SARIF | From |
|---|---|
| `runs[0].tool.driver` | `name` = `purebyte`, `semanticVersion` = runtime version |
| `rules[]` | one per `kind`, in order of first appearance: `id` = `purebyte/<kind>`, `name` = kind, `defaultConfiguration.level` `error`, `properties.model` and `properties.modelVersion` |
| `invocations[0]` | `executionSuccessful` = no failure; each failure is a `toolExecutionNotifications` entry (level `error`, its file as location) |
| `columnKind` | `unicodeCodePoints` |
| `results[]` | one per finding: `ruleId`, `ruleIndex`, `level` = the finding's severity (`error` or `warning`, section 5.1), `message.text` = `possible <kind>: <snippet_masked>`, plus ` (in a test, example or documentation path)` for a warning |
| `locations[0].physicalLocation` | `artifactLocation.uri` (a file URI, percent-encoded); `region` with `startLine`, `startColumn` (code points), `endLine` for text, and `byteOffset`, `byteLength` always; `snippet.text` = the masked value (the clear one with reveal) |
| `partialFingerprints` | `purebyteFinding/v1` = the first 24 hex digits of SHA-256(uri + newline + snippet_masked), then `/` and the occurrence number of that pair: stable across runs, never derived from the clear value |
| `properties` | `confidence`, `votes`, `insideBase64` when true |

## 9. Redaction: output, report and map

Redaction is **by copy**: the output is the input byte for byte except the redacted spans, each replaced by a typed
marker `[TYPE_n]`. Nothing is generated, so nothing can be invented.

- Markers are consistent pseudonyms within one document: the same value (after a per-type normalization: an e-mail in
  any case, a phone number with or without its international prefix, an IBAN with or without spaces, a name with or
  without accents) gets the same marker, and two different values never share one. A phone number keeps all its
  digits but an international prefix (`+`, or `00`, then the E.164 country code); a name or an address keeps its
  letters in any script, Latin ones without their accents (and Greek and Cyrillic ones without theirs), in lower case,
  with runs of white space as one space. Numbers follow the order of appearance and start after the highest
  `[TYPE_n]` already present in the input, so an existing marker is never reused.
- `pb_redact` analyzes the input whatever its length and whatever the profile (a window shorter than the model's
  shortest window included), and refuses an input larger than the profile's limit. When the profile reads a UTF-16
  input as UTF-8 (`secrets-code`), the copy is of the UTF-16 bytes all the same: each span covers the whole
  characters it touches, the markers are written in UTF-16 with the input's byte order, and the report and the map
  carry `"encoding": "utf-16le"` (or `"utf-16be"`) and UTF-16 offsets.
- Overlapping spans are resolved first: spans of one type are joined; of different types, the better span keeps the
  overlap (a span from rules for a type with a strict syntax or checksum — EMAIL, IBAN, CREDIT_CARD, DNI_NIE, IP,
  URL_CREDENTIALS — outranks the others). Empty spans and spans outside the input are ignored. The redacted region is
  the union of the valid spans.
- The copy property (every byte outside the spans is in the output, in order and unchanged) is checked before the
  result is returned.

The **report** (`pb_redaction_report`) never holds a redacted value, nor a hash of one. `input` is the name the caller
gives (`pb_redact_options.name`; empty when none is given, and for `pb_redact_spans`):

```json
{"input": "notes.txt", "model": "pii", "bytes_in": 1204, "bytes_out": 1188,
 "sha256_in": "…", "sha256_out": "…",
 "spans": [{"type": "EMAIL", "start": 118, "end": 139, "marker": "[EMAIL_1]", "confidence": 0.97,
            "source": "model", "out_start": 118, "out_end": 127}],
 "counts": {"EMAIL": 1},
 "guarantee": {"outside_spans_identical": true, "checked": true}}
```

The **map** (`pb_redaction_map`, only with `PB_REDACT_WITH_MAP`) holds the values and must be handled as the secret
it is:

```json
{"version": 1, "input": "notes.txt", "sha256_out": "…",
 "spans": [{"marker": "[EMAIL_1]", "out_start": 118, "out_end": 127, "value_b64": "…"}]}
```

`pb_restore` rebuilds the original from the output and the map by offsets, never by searching for markers (a
marker-like text in the input is harmless). It needs `version` 1 and `sha256_out`, and offsets that are integers
within the output: an output edited after redaction does not match `sha256_out` and is refused, and so is a map
without it.

## 10. Compatibility

Within a major version, results only grow: new fields, new counters, new profiles. Existing fields keep their names,
types and meaning. A consumer MUST ignore unknown fields and SHOULD ignore unknown `kind` values it cannot handle
rather than fail.
