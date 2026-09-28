# pre-commit

Stop credentials before they reach a commit. The hook scans the **staged changes** with a local model and blocks the
commit when it finds a credential on an added line. It takes about a second for a typical commit, and nothing leaves
the machine.

## Set up

1. Install the CLI and the model once per machine ([docs/install.md](../../docs/install.md)):

   ```bash
   pip install purebyte              # or a release binary
   purebyte models pull secrets-code
   ```

2. Add the hook to `.pre-commit-config.yaml` in your repository:

   ```yaml
   repos:
     - repo: https://github.com/purebyte-ai/purebyte
       rev: v1.0.0
       hooks:
         - id: purebyte
   ```

3. Install it: `pre-commit install`. The hook needs pre-commit 3.2 or newer.

The hook definition lives in [`.pre-commit-hooks.yaml`](../../.pre-commit-hooks.yaml) at the root of this
repository, where pre-commit expects it.

**Commits only.** The hook scans what is staged, so `pre-commit run --all-files` (and services that run hooks that
way, such as pre-commit.ci) have nothing to scan and pass. To check a whole repository, run `purebyte scan .`, or use
the [GitHub Action](../github-action/) or the [GitLab CI job](../gitlab-ci/) in your pipeline.

## Options

Extra arguments are appended to `purebyte scan --staged --model secrets-code`:

```yaml
      hooks:
        - id: purebyte
          args: [--threads, "2"]
```

The available options are listed in [docs/cli.md](../../docs/cli.md#scan).

## Test, example and documentation files

The hook keeps the CLI's default: findings in test, example and documentation paths (`tests/`, `src/test/`,
`__tests__/`, `fixtures/`, `examples/`, `docs/`, `*_test.go`, `*.test.js`, `*.md`...) are **warnings**. They are
printed, but they do not block the commit: most of them are credentials made for tests and examples. Only findings
of severity `error` block it. The exact rule is in [docs/cli.md](../../docs/cli.md#severity).

To block on warnings too, or to not scan those paths at all:

```yaml
      hooks:
        - id: purebyte
          args: [--strict]        # or [--tests, "no"]
```

pre-commit shows the output of a hook that passes only with `verbose: true`; add it to the hook to see the warnings
of every commit.

## When it finds something

The commit is stopped and the findings are printed with the values masked. Then:

- **A real credential**: remove it from the change, rotate it (it may already be in your shell history, logs or
  editor backups), and load it from the environment or a secret manager instead.
- **A false alarm**: commit with `git commit --no-verify` if you are sure, and please report the case with the
  "False positive or false negative" issue template, using a look-alike value.

## Without pre-commit

A plain git hook does the same. Save this as `.git/hooks/pre-commit` and make it executable:

```sh
#!/bin/sh
exec purebyte scan --staged --model secrets-code
```

It exits 1, which stops the commit, only on findings of severity `error` (add `--strict` to stop on warnings too).
