# Security policy

PureByte reads sensitive data (credentials, personal data), so we treat vulnerabilities in it with priority.

## Supported versions

| Version | Security fixes |
|---|---|
| Latest minor release (1.0.x at launch) | Yes |
| Older releases | No; please upgrade |

## Reporting a vulnerability

**Please do not open a public issue for a vulnerability.** Report it privately:

1. Preferably through GitHub: the repository's **Security** tab, then **Report a vulnerability**.
2. Or by email to **pablo@purebyte.ai**.

Please include the affected version (`purebyte version`), your platform, the steps or a minimal input that
reproduces the problem, and the impact you expect. **Never include real credentials or real personal data**: use
look-alike values of the same shape. If a proof of concept needs a crafted model or archive file, attach it to the
private report.

What happens next:

- We acknowledge the report within 3 working days and keep you informed as we investigate.
- We agree on a disclosure date with you. By default we aim to publish a fix and an advisory within 90 days.
- We credit reporters in the advisory unless they prefer otherwise.

## What is in scope

- **Data leaving the machine**: any path by which scanned content, findings or credentials could be sent, logged or
  written anywhere that [docs/privacy.md](docs/privacy.md) says they are not.
- **Unmasked output**: a credential printed in clear without the explicit reveal option.
- **Untrusted inputs**: crashes, hangs, excessive memory use or code execution caused by a crafted input file,
  archive (including archive bombs), HTTP request or model file.
- **The local HTTP server**: bypasses of the loopback binding, the `Host` check, the token, or the size limits.
- **Integrations**: the GitHub Action, the pre-commit hook, the GitLab template and the Docker image, including how
  they download and verify binaries and models.
- **Supply chain**: release artifacts, checksums and the model catalog.
- **The training stack and the data scripts** have their own policy, in
  [purebyte-ai/purebyte-train](https://github.com/purebyte-ai/purebyte-train/security/policy).

## What is not a vulnerability

- **A missed credential or a false alarm** is a quality issue, not a security vulnerability (unless it is caused by a
  bug that makes the runtime skip input it claims to have scanned). Please use the "False positive or false negative"
  issue template, with masked snippets only.
- Findings in third-party code that you scanned with PureByte. If you find a live credential in someone else's
  project, please report it privately to that project's owner, not to us.

## Hardening notes for users

- Load model files only from sources you trust, and keep `purebyte models verify` in your pipeline.
- Keep `purebyte serve` on `127.0.0.1` unless you need otherwise, and set a token when you expose it (with
  `--token-file` or `PUREBYTE_SERVE_TOKEN`: a command line is visible to the other users of the machine).
- Pin the versions of the CLI, the models and the GitHub Action in CI.
