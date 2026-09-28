# GitHub Action

Scan every pull request for credentials with a local model and see the findings as **code scanning alerts**, inline
in the pull request diff. The action installs the `purebyte` CLI from a release (checksum and build provenance
verified), pulls the model, scans what changed, uploads a SARIF report and fails the check when it finds a
credential. Findings in test, example and documentation paths are reported as warnings and do not fail it (see
[Severity](#severity)). Your code never leaves the runner.

## Quick start

`.github/workflows/purebyte.yml`:

```yaml
name: PureByte
on:
  pull_request:
  push:
    branches: [main]

permissions:
  contents: read
  security-events: write   # to upload the SARIF report to code scanning

jobs:
  secrets:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v7
      - uses: purebyte-ai/purebyte/integrations/github-action@v1
```

On a pull request it scans the changes against the base branch and reports findings on added lines; in a merge queue
(`merge_group`), the changes of the group against its base; on a push, the changes the push introduces. Findings
appear in the **Security** tab and as annotations on the changed lines, with values masked.

## Inputs

| Input | Default | Meaning |
|---|---|---|
| `model` | `secrets-code` | Specialist to run. Use one step per specialist (see below) |
| `version` | the action's release | CLI version to install. By default, the version of the release the action is pinned to (`@v1.0.0` installs 1.0.0; `@v1`, the release that tag points to), so that pinning the action pins the CLI and the models it knows. `latest` follows the newest release; a version such as `1.0.0` installs that one |
| `verify-provenance` | `true` | Verify the build provenance attestation of the downloaded archive (`gh attestation verify`), besides its checksum |
| `path` | `.` | Directory to scan |
| `scope` | `changed` | `changed`: only what the pull request or push changes, findings on added lines. `all`: everything under `path` |
| `sarif-file` | `purebyte.sarif` | Where to write the report |
| `upload-sarif` | `true` | Upload the report to code scanning (`args: --reveal` is then refused: the clear values would be stored in the alerts) |
| `fail-on-findings` | `true` | Fail the step when there is at least one finding of severity `error` |
| `strict` | `false` | Fail on warnings too: findings in test, example and documentation paths (`purebyte scan --strict`) |
| `args` | | Extra arguments for `purebyte scan` ([docs/cli.md](../../docs/cli.md#scan)) |
| `github-token` | `${{ github.token }}` | Used to download the release, verify its attestation and, with `version: latest`, find it |

## Outputs

| Output | Meaning |
|---|---|
| `findings` | Number of findings, errors and warnings |
| `errors` | Number of findings of severity `error`: the ones that fail the step |
| `warnings` | Number of warnings: findings in test, example and documentation paths |
| `sarif-file` | Path of the SARIF report |

## Severity

The action keeps the CLI's default: a finding is an `error`, except in test, example and documentation paths
(`tests/`, `src/test/`, `__tests__/`, `fixtures/`, `examples/`, `docs/`, `*_test.go`, `*.test.js`, `*.md`...), where
it is a `warning`. Most of what a secrets model finds there are credentials made for tests and examples, and failing
every pull request on them buries the real ones. Warnings are uploaded with SARIF level `warning`, so they show in
code scanning next to the errors, and they are counted in the step summary; only errors fail the step. The rule and
its exact list of paths are in [docs/cli.md](../../docs/cli.md#severity).

To fail on warnings too, set `strict: true`. To not scan those paths at all, add `args: --tests no`.

## Recipes

**Binaries too.** Build first, then scan the artifacts with `secrets-bin` in a second step:

```yaml
      - uses: purebyte-ai/purebyte/integrations/github-action@v1
        with:
          model: secrets-bin
          path: dist
          scope: all
          sarif-file: purebyte-bin.sarif
```

**Test credentials count too**, for a repository whose tests must hold no credential at all:

```yaml
      - uses: purebyte-ai/purebyte/integrations/github-action@v1
        with:
          strict: true
```

**Report without failing**, for example while you clean up an existing repository:

```yaml
      - uses: purebyte-ai/purebyte/integrations/github-action@v1
        with:
          fail-on-findings: false
```

**A weekly full scan:**

```yaml
on:
  schedule:
    - cron: "0 3 * * 1"
jobs:
  secrets:
    runs-on: ubuntu-latest
    permissions:
      contents: read
      security-events: write
    steps:
      - uses: actions/checkout@v7
      - uses: purebyte-ai/purebyte/integrations/github-action@v1
        with:
          scope: all
```

## Notes

- **Forks.** Pull requests from forks get a read-only token that cannot upload SARIF, so the action skips the upload
  for them; the scan still runs and still fails the check on errors.
- **Private repositories** need GitHub code scanning to be available on the plan to show alerts. Without it, set
  `upload-sarif: false` and rely on the log and the check status.
- **Supply chain.** Pin the action to a release tag or a commit SHA: the CLI it installs follows (see `version`). The
  installer verifies the SHA-256 checksum of the archive and its build provenance attestation, a signed statement
  that the release workflow of `purebyte-ai/purebyte` built it from that version's tag (a checksum stored next to the
  archive only detects corruption), and `purebyte models pull` verifies the model.
- **`pull_request_target` is refused.** That event runs on the base branch with a write token: scanning its checkout
  would say nothing about the pull request, and checking out the pull request there would run untrusted content with
  that token. Use `pull_request`.
- **Runners.** Linux, macOS and Windows runners, x86-64 and arm64 (on Windows on Arm, the x86-64 build runs under
  emulation: there is no Windows arm64 build). The steps need `bash`, `curl`, `git` and Python 3 (`python3` or
  `python`), plus `pwsh` on Windows and the GitHub CLI (`gh`) to verify the provenance: GitHub-hosted runners have them
  all; on a self-hosted runner, install them (or set `verify-provenance: false`).
- **Inputs that cannot be scanned**, such as files over the model's size limit (4,000,000 bytes for `secrets-code`),
  fail the step after the findings are reported and uploaded; each one is listed in the log with its reason.

## Show it

Add the badge to your README: see [integrations/README.md](../README.md#scanned-by-purebyte-badge).
