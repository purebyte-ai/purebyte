#!/usr/bin/env bash
# Installs the purebyte CLI from a GitHub release, and verifies the archive twice before running it: its SHA-256
# checksum (a corrupt download) and its build provenance attestation (an archive that the release workflow of the
# repository did not build: a checksum stored next to the archive cannot tell).
#
# Environment: PUREBYTE_VERSION (a version such as 1.0.0, `latest`, or empty for the version of the release this
# action comes from), PUREBYTE_REPO (owner/repository of the releases; defaults to the repository of this action),
# PUREBYTE_VERIFY_PROVENANCE (`true` or `false`), GH_TOKEN (the GitHub API: attestations, and `latest`).
# Release assets are named purebyte-<version>-<os>-<arch>.<tar.gz|zip>, each with a .sha256 file next to it.
set -euo pipefail

repo=${PUREBYTE_REPO:-purebyte-ai/purebyte}
version=${PUREBYTE_VERSION:-}
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

py() { if command -v python3 > /dev/null 2>&1; then python3 "$@"; else python "$@"; fi; }
fetch() {  # fetch URL [OUTPUT]; authenticated when a token is available
  if [ -n "${GH_TOKEN:-}" ]; then
    curl -fsSL -H "Authorization: Bearer ${GH_TOKEN}" ${2:+-o "$2"} "$1"
  else
    curl -fsSL ${2:+-o "$2"} "$1"
  fi
}

if [ -z "$version" ]; then
  # The version of the release this action comes from: pinning the action (@v1.0.0, a commit) pins the CLI and the
  # model catalog built into it. The action runs from a checkout of the whole repository at that reference.
  version=$(sed -n 's/^project(purebyte VERSION \([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\).*/\1/p' \
    "$here/../../CMakeLists.txt" 2> /dev/null || true)
  if [ -z "$version" ]; then
    echo "::error::cannot tell the version of this action: set the \`version\` input (for example 1.0.0)"
    exit 1
  fi
elif [ "$version" = latest ]; then
  # The highest vX.Y.Z release, not GitHub's "latest release": model weights are released in the same repository,
  # under tags such as secrets-code-1.0.0 (docs/releasing.md).
  version=$(fetch "https://api.github.com/repos/${repo}/releases?per_page=100" | py -c '
import json, re, sys
found = [tuple(int(x) for x in m.groups()) for r in json.load(sys.stdin) if not r["draft"] and not r["prerelease"]
         for m in [re.fullmatch(r"v(\d+)\.(\d+)\.(\d+)", r["tag_name"])] if m]
if not found:
    sys.exit("::error::no release named vX.Y.Z in " + sys.argv[1])
print("%d.%d.%d" % max(found))' "$repo")
fi
if ! [[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "::error::\`version\` is a version such as 1.0.0, or latest: not ${version}"
  exit 1
fi

case "${RUNNER_OS:-$(uname -s)}" in
  Linux) os=linux; ext=tar.gz ;;
  macOS | Darwin) os=macos; ext=tar.gz ;;
  Windows | MINGW* | MSYS*) os=windows; ext=zip ;;
  *) echo "::error::unsupported operating system: ${RUNNER_OS:-unknown}"; exit 1 ;;
esac
case "${RUNNER_ARCH:-$(uname -m)}" in
  X64 | x86_64 | amd64) arch=x86_64 ;;
  ARM64 | arm64 | aarch64) arch=arm64 ;;
  *) echo "::error::unsupported architecture: ${RUNNER_ARCH:-unknown}"; exit 1 ;;
esac
# Windows on Arm runs the x86-64 build under emulation: there is no windows-arm64 archive.
if [ "$os" = windows ] && [ "$arch" = arm64 ]; then arch=x86_64; fi

asset="purebyte-${version}-${os}-${arch}.${ext}"
url="https://github.com/${repo}/releases/download/v${version}/${asset}"
dir="${RUNNER_TEMP:-/tmp}/purebyte-${version}"
mkdir -p "$dir"
cd "$dir"

echo "Downloading ${asset} from ${repo}"
fetch "$url" "$asset"
fetch "${url}.sha256" "${asset}.sha256"

expected=$(cut -d ' ' -f 1 < "${asset}.sha256")
if command -v sha256sum > /dev/null 2>&1; then
  actual=$(sha256sum "$asset" | cut -d ' ' -f 1)
else
  actual=$(shasum -a 256 "$asset" | cut -d ' ' -f 1)
fi
if [ "$expected" != "$actual" ]; then
  echo "::error::checksum mismatch for ${asset}: expected ${expected}, got ${actual}"
  exit 1
fi

# The provenance: a signed statement (Sigstore) that the release workflow of the repository built this archive from
# the tag v<version>, on a GitHub-hosted runner. Whoever could replace a release asset (and its checksum) cannot sign
# one.
if [ "${PUREBYTE_VERIFY_PROVENANCE:-true}" = true ]; then
  if ! command -v gh > /dev/null 2>&1; then
    echo "::error::gh (the GitHub CLI) is needed to verify the provenance of ${asset}: install it on this runner, or set verify-provenance: false"
    exit 1
  fi
  gh attestation verify "$asset" --repo "$repo" --signer-workflow "${repo}/.github/workflows/release.yml" \
    --source-ref "refs/tags/v${version}" --deny-self-hosted-runners > /dev/null
  echo "Verified: ${asset} was built by ${repo}/.github/workflows/release.yml from v${version}"
else
  echo "::warning::the provenance of ${asset} was not verified (verify-provenance: false); its checksum was"
fi

if [ "$ext" = zip ]; then
  pwsh -NoProfile -Command "Expand-Archive -Force -Path '${asset}' -DestinationPath 'bin'"
  binary=$(find bin -name purebyte.exe -type f | head -n 1)
else
  mkdir -p bin
  tar -xzf "$asset" -C bin
  binary=$(find bin -name purebyte -type f | head -n 1)
fi
if [ -z "$binary" ]; then
  echo "::error::no purebyte executable found in ${asset}"
  exit 1
fi
chmod +x "$binary" 2> /dev/null || true

bindir=$(cd "$(dirname "$binary")" && pwd)
# Later steps may run another shell than bash (pwsh, cmd): on Windows, GITHUB_PATH takes a Windows path.
if [ "$os" = windows ] && command -v cygpath > /dev/null 2>&1; then bindir=$(cygpath -w "$bindir"); fi
echo "$bindir" >> "${GITHUB_PATH:-/dev/null}"
"$binary" version
