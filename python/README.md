# purebyte

**Tiny byte-level AI models on your CPU.** PureByte is an architecture and a runtime for AI Specialists: models of 1
to 29 MiB that read raw bytes and answer with decisions, scores and exact byte ranges instead of generated text. A
short decision takes 0.8 to 1.5 ms on a 12-core desktop CPU (3 to 6 ms on one thread), offline, with the same bits
from every CPU kernel and thread count of a platform, and the same decisions and spans expected across platforms.

This package bundles the native PureByte runtime and its `purebyte` command, with thin Python bindings (standard
library only). It runs any PureByte model file, including the three example specialists of the first release: `pii`
(masks personal data), `secrets-code` (finds credentials in code) and `secrets-bin` (finds credentials in binaries).

## Install

```bash
pip install purebyte
purebyte models pull pii                 # an example specialist, checked against its SHA-256
```

## Command line

```bash
purebyte redact --model pii ticket.txt           # personal data masked, the rest copied byte for byte
purebyte scan --model secrets-code .             # leaked credentials in a repository
purebyte scan --model my-model.gguf data/        # any model you trained
purebyte bench --model pii                       # latency and throughput on this machine
```

## Python

```python
import purebyte

redaction = purebyte.redact(open("ticket.txt", "rb").read(), model="pii")
print(redaction.output.decode())

result = purebyte.scan("settings.py", model="secrets-code")
for finding in result.findings:
    print(finding.file, finding.line, finding.col, finding.snippet_masked)
```

Detected values are masked in every output unless you explicitly ask to reveal them.

## Learn more

- The architecture, the examples, benchmarks and documentation: <https://github.com/purebyte-ai/purebyte>
- Train your own specialist: <https://github.com/purebyte-ai/purebyte-train/blob/main/docs/training.md>
- The Python API: <https://github.com/purebyte-ai/purebyte/blob/main/docs/api.md#python>
- Licenses: the code is Apache-2.0; the released model weights have their own license
  (<https://github.com/purebyte-ai/purebyte/blob/main/MODEL_LICENSE.md>).
