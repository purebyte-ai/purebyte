# Privacy and data handling

PureByte exists so that sensitive bytes never have to leave your machine to be analyzed. This page lists exactly what
it does with your data. If anything here is not true of the code, that is a security bug: please report it as
described in [SECURITY.md](../SECURITY.md).

## The short version

- **Nothing you scan is sent anywhere.** Models run in the process, on your CPU.
- **No telemetry.** No usage statistics, no crash reports, no update checks.
- **Credentials are never verified.** PureByte does not contact any provider to check whether a secret works.
- **Findings are masked by default**, in every output format, including SARIF files that you upload elsewhere.

## Network access

The only command that uses the network is `purebyte models pull`, which downloads model files (with `curl`, over
HTTPS) from the URLs listed in [models/models.json](../models/models.json) and checks their sizes and SHA-256
checksums. Everything else works offline, including on air-gapped machines once the models are installed.

`purebyte serve` opens a listening socket on `127.0.0.1` by default. It sends nothing on its own; it only answers the
requests you send to it.

## Credentials are never verified

Some secret scanners confirm a finding by trying the credential against the provider's API. That sends the secret
to a third party and leaves a trace in its logs. PureByte never does this: a finding is a statement about the bytes,
not a login attempt. (For comparison, the trufflehog runs in our benchmarks used `--no-verification` for the same
reason.)

## Masking

Every finding carries a masked snippet instead of the value:

| Value | Masked as |
|---|---|
| 16 characters or more | First four and last two characters kept: `ghp_************8D` |
| 8 to 15 characters | First two characters kept: `Tr************` |
| Fewer than 8 characters | Fully hidden |
| Private key block | The header line as it is, which names the key type (for example `BEGIN RSA PRIVATE KEY` between its dashes), then the line count: `… (15 lines)` |

At most twelve asterisks are shown, so a mask does not reveal the length of a long value. The unmasked value appears
only when you ask for it explicitly (see [cli.md](cli.md)), in any output format including SARIF.

## Redaction

`purebyte redact` writes a copy of a file with every detected value replaced by a typed marker, and checks that
nothing else changed. Its report holds types, offsets and markers, never the values. The optional map that restores
the original does hold the values: it is written only when you ask for it, and on Linux and macOS the file is created
readable by its owner only.

## The local HTTP server

| Protection | Default |
|---|---|
| Listening address | `127.0.0.1` only; binding to another interface must be requested |
| DNS rebinding | On loopback, requests whose `Host` header is not `127.0.0.1`, `localhost` or `[::1]` are rejected, so a web page cannot reach the server through a malicious domain |
| Authentication | Optional bearer token; when set, every endpoint except `/health` requires it |
| Disk | The server writes nothing to disk: request bodies, archive members and results live in memory |
| Logs | The access log records method, path, status and sizes, never content |
| Paths | The server never opens a file path received in a request; it only scans the bytes sent to it |
| Size limits | Bodies above a configurable limit are rejected |

Details and options: [api.md](api.md#security).

## Model files

A model is data, but a crafted file is still untrusted input. The engine validates every model it loads (format,
declared architecture, tensor shapes, and that every tensor in the file is used), refuses files it does not fully
understand instead of guessing, and `purebyte models pull` and `purebyte models verify` check SHA-256 checksums.
Load models only from sources you trust.

## The training stack

Training is separate from the runtime, in its own repository,
[purebyte-train](https://github.com/purebyte-ai/purebyte-train), and runs only when you start it. Its scripts in
[data/](https://github.com/purebyte-ai/purebyte-train/blob/main/data/README.md) download public datasets from the
sources they list, pinned by commit or by SHA-256, and verify what they get; they upload nothing. Datasets, runs and
trained models stay on your machine, outside the source tree (`$PUREBYTE_DATA`, `$PUREBYTE_WORK`). An AI coding agent that drives the training (Vibe Training) sees what you let it see: the privacy
of that conversation is the agent's, not PureByte's.

## Integrations

- The [GitHub Action](../integrations/github-action/) uploads a SARIF file to GitHub code scanning, which then shows
  the findings to people with access to the repository. Snippets are masked.
- CI logs contain the masked findings printed by the CLI. Anyone who can read your CI logs can read those.
- The [Docker image](../integrations/docker/) contains no model; models are pulled at run time or mounted from a
  volume.

## When you report a problem to us

Issue templates ask for **masked** snippets only. Never paste a real credential or real personal data in an issue,
a pull request or a discussion: replace the value with a look-alike of the same shape (see
[CONTRIBUTING.md](../CONTRIBUTING.md#report-a-false-positive-or-a-missed-secret)).
