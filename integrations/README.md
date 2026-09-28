# Integrations

Put PureByte in your workflow: before the commit, in the pull request, in the pipeline and in the editor. The
ready-made integrations run the `secrets-code` example, and take any other model with the same option (`model:` or
`--model`). Every integration runs the same local CLI, so nothing leaves the machine or the runner.

| Where | What you get | Guide |
|---|---|---|
| Before each commit | Staged changes scanned in about a second; the commit is stopped on a credential (test and example paths only warn) | [pre-commit](pre-commit/) |
| GitHub pull requests | Findings as code scanning alerts, inline in the diff | [GitHub Action](github-action/) |
| GitLab merge requests | A ready-made job with model caching and a SARIF artifact | [GitLab CI](gitlab-ci/) |
| Any CI or container platform | The runtime in a small image; models pulled at run time | [Docker](docker/) |
| VS Code | Tasks to scan the current file, the staged changes or the workspace | [VS Code](vscode/) |
| Your own code | The local HTTP API, the Python package or the C library | [docs/api.md](../docs/api.md) |

## What a finding looks like in code scanning

The GitHub Action uploads a SARIF 2.1.0 report. Each finding becomes an alert on the exact line, with the value
masked, a stable fingerprint (so an alert is tracked across commits instead of reappearing) and the number of
ensemble votes. Its level is the finding's severity: `error`, or `warning` in test, example and documentation paths.
The mapping from findings to SARIF is defined in [spec/OUTPUT.md](../spec/OUTPUT.md).

## "Scanned by PureByte" badge

[![scanned by PureByte](https://img.shields.io/badge/scanned%20by-PureByte-C6F16B?labelColor=0D0F13&logo=data%3Aimage%2Fsvg%2Bxml%3Bbase64%2CPHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCA0OCA0OCI%2BPHBhdGggZD0iTTUuODAgMTYuMjBBMTAuNCAxMC40IDAgMSAwIDI2LjYwIDE2LjIwQTEwLjQgMTAuNCAwIDEgMCA1LjgwIDE2LjIwWk0zMS40MCAxNi4yMEE1LjQgNS40IDAgMSAwIDQyLjIwIDE2LjIwQTUuNCA1LjQgMCAxIDAgMzEuNDAgMTYuMjBaTTEwLjgwIDM2LjgwQTUuNCA1LjQgMCAxIDAgMjEuNjAgMzYuODBBNS40IDUuNCAwIDEgMCAxMC44MCAzNi44MFpNMzEuNDAgMzYuODBBNS40IDUuNCAwIDEgMCA0Mi4yMCAzNi44MEE1LjQgNS40IDAgMSAwIDMxLjQwIDM2LjgwWiIgZmlsbD0iI0M2RjE2QiIvPjwvc3ZnPg%3D%3D)](https://github.com/purebyte-ai/purebyte)

Using one of the integrations above? Tell your users by adding this to your README:

```markdown
[![scanned by PureByte](https://img.shields.io/badge/scanned%20by-PureByte-C6F16B?labelColor=0D0F13&logo=data%3Aimage%2Fsvg%2Bxml%3Bbase64%2CPHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCA0OCA0OCI%2BPHBhdGggZD0iTTUuODAgMTYuMjBBMTAuNCAxMC40IDAgMSAwIDI2LjYwIDE2LjIwQTEwLjQgMTAuNCAwIDEgMCA1LjgwIDE2LjIwWk0zMS40MCAxNi4yMEE1LjQgNS40IDAgMSAwIDQyLjIwIDE2LjIwQTUuNCA1LjQgMCAxIDAgMzEuNDAgMTYuMjBaTTEwLjgwIDM2LjgwQTUuNCA1LjQgMCAxIDAgMjEuNjAgMzYuODBBNS40IDUuNCAwIDEgMCAxMC44MCAzNi44MFpNMzEuNDAgMzYuODBBNS40IDUuNCAwIDEgMCA0Mi4yMCAzNi44MEE1LjQgNS40IDAgMSAwIDMxLjQwIDM2LjgwWiIgZmlsbD0iI0M2RjE2QiIvPjwvc3ZnPg%3D%3D)](https://github.com/purebyte-ai/purebyte)
```

## Missing your platform?

The CLI is all an integration needs: `purebyte scan` with `--staged` or `--git-diff REF` for changes, `--format
sarif` for security dashboards, and exit status 0 (no error), 1 (findings of severity `error`; with `--strict`, any
finding) or 2 (the scan could not complete). Findings in test, example and documentation paths are warnings
([docs/cli.md](../docs/cli.md#severity)): report them, but let only errors fail the job. Open a feature request, or a
pull request that adds a folder here with a README like the others.
