# Docker

The official image contains the `purebyte` CLI built from this repository, and no model. Models are pulled at run
time into `/models`; mount a named volume there so they are downloaded once.

```bash
docker volume create purebyte-models
docker run --rm -v purebyte-models:/models ghcr.io/purebyte-ai/purebyte:1.0.0 models pull secrets-code

# scan the current directory (read-only mount)
docker run --rm -v purebyte-models:/models -v "$PWD:/src:ro" ghcr.io/purebyte-ai/purebyte:1.0.0 \
  scan --model secrets-code /src
```

The container runs as an unprivileged user, needs no network access except for `models pull`, and writes nothing
outside `/models`. Add `--network none` to the scan command to make that explicit.

For `--staged` and `--git-diff`, mount the repository at `/src`: git trusts the configuration of a repository that
another user owns only there. Anywhere else, git refuses to work unless you trust that path yourself, for example with
`--entrypoint sh ... -c 'git config --global --add safe.directory /work && purebyte scan --git-diff origin/main /work'`.

## Local HTTP API

```bash
docker run --rm -v purebyte-models:/models -p 127.0.0.1:8421:8421 ghcr.io/purebyte-ai/purebyte:1.0.0 \
  serve --model secrets-code --host 0.0.0.0
```

Inside the container the server must listen on `0.0.0.0` to be reachable; publishing the port on `127.0.0.1` keeps
it private to the host. Without a token the server still checks the `Host` header, as on loopback: it answers
requests addressed to `127.0.0.1` or `localhost`, so a web page that points a domain of its own at your machine (DNS
rebinding) is refused. Set a token (see [docs/api.md](../../docs/api.md#security)) if other machines can reach it,
through the environment rather than the command line: `docker run -e PUREBYTE_SERVE_TOKEN ...` passes the variable of
your shell to the server, and the server refuses to start if it is set but empty.

## Build the image yourself

From the root of the repository:

```bash
docker build -f integrations/docker/Dockerfile -t purebyte .
```

The build compiles the runtime in a full Debian image, runs the test suite, and copies only the binary (with its
licenses, in `/usr/share/doc/purebyte`) into a slim runtime image with `git` (for diff scans), and `curl` and CA
certificates (for model downloads).

## Offline machines

Pull the models on a connected machine, then copy the volume's contents (or the `.gguf` files) to the offline
machine and run `purebyte models verify` there. See [docs/install.md](../../docs/install.md#models).
