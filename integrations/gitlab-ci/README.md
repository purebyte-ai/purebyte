# GitLab CI

A ready-made job that scans merge requests and pushes for credentials with a local model, using the official Docker
image. Models are cached between pipelines, and nothing leaves the runner.

## Set up

Add to your `.gitlab-ci.yml`:

```yaml
include:
  - remote: "https://raw.githubusercontent.com/purebyte-ai/purebyte/v1.0.0/integrations/gitlab-ci/purebyte.gitlab-ci.yml"
```

The job, named `purebyte`, runs in the `test` stage:

| Pipeline | What is scanned |
|---|---|
| Merge request | The changes against the target branch; findings on added lines only |
| Branch push | The changes of the push |
| Anything else (new branch, schedule, manual) | The whole repository |

It fails when it finds a credential, and when an input could not be scanned (for example a file over the model's size
limit: the reason is in the report). Findings in test, example and documentation paths are warnings: they are in the
log and the report, but they do not fail the job unless `PUREBYTE_ARGS` has `--strict`
([docs/cli.md](../../docs/cli.md#severity)). The SARIF report is kept as the `purebyte.sarif` artifact for 30 days.

## Variables

| Variable | Default | Meaning |
|---|---|---|
| `PUREBYTE_MODEL` | `secrets-code` | Specialist to run |
| `PUREBYTE_PATH` | `.` | What to scan |
| `PUREBYTE_SCOPE` | `changed` | `changed` as in the table above; `all` always scans everything under `PUREBYTE_PATH` |
| `PUREBYTE_ARGS` | | Extra arguments for `purebyte scan` ([docs/cli.md](../../docs/cli.md#scan)), split at spaces and never expanded as file patterns (`--exclude *.min.js` stays a pattern) |
| `PUREBYTE_FAIL_ON_FINDINGS` | `true` | Set to `false` to report without failing |
| `PUREBYTE_IMAGE` | `ghcr.io/purebyte-ai/purebyte:1.0.0` | The image to run; pin it to a digest for stricter supply-chain control |

Override them where you include the template, or in the project's CI/CD settings.

## Scanning build artifacts

Extend the job for binaries produced by an earlier stage:

```yaml
purebyte-binaries:
  extends: purebyte
  needs: [build]
  variables:
    PUREBYTE_MODEL: "secrets-bin"
    PUREBYTE_PATH: "dist"
    PUREBYTE_SCOPE: "all"
```

## Notes

- GitLab's security dashboards do not read SARIF directly; the findings are in the job log (masked) and in the
  artifact.
- `GIT_DEPTH: "0"` fetches the full history so that the target branch can be compared; on very large repositories,
  a shallower depth that still contains the merge base works too.
- The model cache (`.purebyte/` in the project folder, where GitLab keeps caches) is left out of every scan.
- The image runs as an unprivileged user, and git refuses the configuration of a repository that another user owns:
  the job marks its own checkout as trusted (`git config --global --add safe.directory "$CI_PROJECT_DIR"`), and
  nothing else.
