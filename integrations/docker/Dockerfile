# syntax=docker/dockerfile:1
#
# PureByte runtime image. Build from the root of the repository:
#
#   docker build -f integrations/docker/Dockerfile -t purebyte .
#
# The image contains the `purebyte` CLI and no model: models are pulled at run time into /models (mount a volume
# there to keep them between runs). See integrations/docker/README.md.
#
# The base images are pinned by digest (the multi-architecture index), so that a build is reproducible; Dependabot
# proposes new digests (.github/dependabot.yml).

# ---------------------------------------------------------------------------------------------------------- build
FROM debian:bookworm@sha256:f37a335e82bca302e955fa39f9dfe28f1be618f016f8a2b56318e5a5111afc26 AS build

RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates cmake g++ make python3 \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# Release build, with the test suite as a gate: a broken engine never becomes an image.
RUN cmake -S . -B /build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build /build --parallel \
 && ctest --test-dir /build --output-on-failure \
 && install -D -m 0755 "$(find /build -type f -name purebyte -perm -u+x | head -n 1)" /out/purebyte

# -------------------------------------------------------------------------------------------------------- runtime
FROM debian:bookworm-slim@sha256:3783cc01769c7b2b1b83a5c5ad96c815348e28ed7da68e2e3687004faa906251

LABEL org.opencontainers.image.title="PureByte" \
      org.opencontainers.image.description="Open runtime for tiny byte-level AI models. Models are pulled at run time." \
      org.opencontainers.image.licenses="Apache-2.0" \
      org.opencontainers.image.source="https://github.com/purebyte-ai/purebyte"

# git: scanning diffs (--git-diff, --staged). curl and ca-certificates: `purebyte models pull` downloads with curl
# over HTTPS. git trusts the configuration of the repository mounted at /src, which belongs to another user than the
# container's; any other repository keeps git's ownership check (`git config --global --add safe.directory PATH`
# trusts one more, as the GitLab CI job does for its checkout).
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl git \
 && rm -rf /var/lib/apt/lists/* \
 && git config --system --add safe.directory /src \
 && useradd --create-home --uid 10001 purebyte \
 && mkdir -p /models /src \
 && chown purebyte:purebyte /models

COPY --from=build /out/purebyte /usr/local/bin/purebyte
# The licenses that go with the binary: the project's, and those of the vendored libraries (cli/third_party).
COPY LICENSE /usr/share/doc/purebyte/LICENSE
COPY cli/third_party/cpp-httplib/LICENSE /usr/share/doc/purebyte/LICENSE-cpp-httplib

# Models go to /models: mount a named volume there to keep them (no VOLUME here, which would leave an anonymous
# volume behind every `docker run` without one).
ENV PUREBYTE_HOME=/models
USER purebyte
WORKDIR /src

ENTRYPOINT ["purebyte"]
CMD ["--help"]
