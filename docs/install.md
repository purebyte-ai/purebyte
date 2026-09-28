# Install

PureByte is one self-contained program, `purebyte`, plus the model files of the AI Specialists you use. Pick the
way that suits you:

| Way | Best for | Command |
|---|---|---|
| [pip](#pip) | Python users, quick trials | `pip install purebyte` |
| [Release binary](#release-binaries) | Servers, CI, anyone without Python | Download, verify, unpack |
| [Docker](#docker) | Containers and CI platforms | `docker pull ghcr.io/purebyte-ai/purebyte:1.0.0` |
| [From source](#from-source) | Contributors, other platforms | `cmake` |

Then [install a model](#models).

## pip

```bash
pip install purebyte
purebyte version
```

The package brings the `purebyte` command and the Python module (`import purebyte`); it has no Python dependencies.
It is published as wheels for Python 3.8 or newer on the platforms of the release binaries below: Linux x86-64 and
arm64 with glibc 2.28 or newer (manylinux_2_28), macOS 11 or newer, and Windows x86-64. There is no source package on
PyPI, so elsewhere (musl-based Linux such as Alpine, an older glibc, Windows on Arm) `pip` finds no matching
distribution: [build from source](#from-source).

## Release binaries

Every [release](https://github.com/purebyte-ai/purebyte/releases) publishes one archive per platform, each with a SHA-256
checksum file next to it:

| Platform | Asset |
|---|---|
| Linux x86-64 | `purebyte-<version>-linux-x86_64.tar.gz` |
| Linux arm64 | `purebyte-<version>-linux-arm64.tar.gz` |
| macOS arm64 (Apple silicon) | `purebyte-<version>-macos-arm64.tar.gz` |
| macOS x86-64 | `purebyte-<version>-macos-x86_64.tar.gz` |
| Windows x86-64 | `purebyte-<version>-windows-x86_64.zip` |

Linux and macOS:

```bash
version=1.0.0
asset=purebyte-$version-linux-x86_64.tar.gz      # pick yours from the table
curl -fsSLO https://github.com/purebyte-ai/purebyte/releases/download/v$version/$asset
curl -fsSLO https://github.com/purebyte-ai/purebyte/releases/download/v$version/$asset.sha256
sha256sum -c $asset.sha256                       # macOS: shasum -a 256 -c $asset.sha256
gh attestation verify $asset --repo purebyte-ai/purebyte   # optional, with the GitHub CLI: built by the release workflow
mkdir -p purebyte-$version && tar -xzf $asset -C purebyte-$version   # purebyte, LICENSE, LICENSE-cpp-httplib
sudo install purebyte-$version/purebyte /usr/local/bin/   # or any directory on your PATH
```

Windows (PowerShell):

```powershell
$version = "1.0.0"
$asset = "purebyte-$version-windows-x86_64.zip"
Invoke-WebRequest "https://github.com/purebyte-ai/purebyte/releases/download/v$version/$asset" -OutFile $asset
Invoke-WebRequest "https://github.com/purebyte-ai/purebyte/releases/download/v$version/$asset.sha256" -OutFile "$asset.sha256"
(Get-FileHash $asset -Algorithm SHA256).Hash.ToLower() -eq (Get-Content "$asset.sha256").Split(" ")[0]   # must print True
Expand-Archive $asset -DestinationPath "$env:LOCALAPPDATA\purebyte\bin"
# then add $env:LOCALAPPDATA\purebyte\bin to your PATH
```

## Docker

```bash
docker pull ghcr.io/purebyte-ai/purebyte:1.0.0
```

The image holds the CLI and no model. How to keep models in a volume and scan a directory:
[integrations/docker](../integrations/docker/README.md).

## From source

Requirements: CMake 3.20 or newer and a C++17 compiler: GCC, Clang, MinGW-w64, or on Windows MSVC 19.14 (Visual
Studio 2017 version 15.7) or newer and clang-cl. The runtime has no other dependencies. The build turns floating-point
contraction off on every one of them, so they all compute the same bits (the numerics contract,
[FORMAT.md](../spec/FORMAT.md) section 5).

```bash
git clone https://github.com/purebyte-ai/purebyte.git
cd purebyte
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build --build-config Release --output-on-failure
cmake --install build --prefix "$HOME/.local"      # bin/purebyte, the library, include/purebyte/pb.h, licenses
```

CMake options: `PUREBYTE_BUILD_CLI`, `PUREBYTE_BUILD_TESTS` and `PUREBYTE_BUILD_SHARED` (all on by default), and
`PUREBYTE_STATIC_RUNTIME` (off by default; release binaries turn it on). The test suite needs Python 3.8 or newer for
its model-based tests, and NumPy for the comparison with the reference implementation (skipped without it).

## Models

```bash
purebyte models list                   # the catalog, and what is installed
purebyte models pull secrets-code      # download and verify the SHA-256 checksum
purebyte models pull secrets-bin
purebyte models verify                 # re-check every installed model file
```

`pull` downloads with `curl`, which Windows 10 version 1803 and later, macOS and most Linux distributions include,
but slim container images (such as `python:*-slim` or `debian:*-slim`) do not; it must be in a folder of `PATH` (the
current folder is never searched). It is the only command that connects to the network; `purebyte serve` listens on
loopback by default. Models are stored in a per-user directory, or in `PUREBYTE_HOME` when it is set (the Docker image
uses `/models`):

| System | Directory |
|---|---|
| Linux | `$XDG_DATA_HOME/purebyte/models`, or `~/.local/share/purebyte/models` |
| macOS | `~/Library/Application Support/purebyte/models` |
| Windows | `%LOCALAPPDATA%\purebyte\models` |

`purebyte info` shows the directory in use and the CPU kernel. Where none of these variables is set (some service
managers and cron jobs set no `HOME`), set `PUREBYTE_HOME`: the CLI never falls back to a folder relative to the
current one.

**Offline machines.** On a connected machine, download the `.gguf` files listed in
[models/models.json](../models/models.json) from the release page. Copy them to the models directory of the offline
machine and run `purebyte models verify`, which checks them against the checksums built into the CLI. A model file can
also be passed directly by path wherever a model name is accepted (a name ending in `.gguf`, or a path with a folder
such as `./my-model`).

**A local mirror.** `PUREBYTE_CATALOG` points the CLI to another catalog file with the same schema as
[models/models.json](../models/models.json); with `file://` URLs in it, `purebyte models pull` installs and verifies
the files from a shared folder. Each `file` of such a catalog must stay a plain file name ending in `.gguf`: a catalog
that gives a path there (`../x.gguf`, an absolute path) is refused, since `pull` writes where it says.

## Check the installation

```bash
purebyte version
purebyte info
echo 'password = "Tr0ub4dor&3xyz"' | purebyte scan -      # a made-up password: one finding, exit status 1
```

## Supported platforms

| Operating system | Architectures | CPU kernel |
|---|---|---|
| Linux | x86-64, arm64 | AVX2 on x86-64 CPUs that have it, NEON on arm64, portable fallback elsewhere |
| macOS 11 or newer | arm64, x86-64 | NEON on Apple silicon, AVX2 on Intel |
| Windows 10 or newer | x86-64 | AVX2 when available |

Other little-endian CPUs build from source and run the portable kernel. Big-endian CPUs are not supported: model
files are little-endian, and the reader does not swap bytes.

Building from source needs a compiler with the C++17 standard library, `std::filesystem` included. On macOS the build
targets macOS 11.0 by default, like the release binaries; `-DCMAKE_OSX_DEPLOYMENT_TARGET=10.15` (the oldest release
with `std::filesystem`) or the `MACOSX_DEPLOYMENT_TARGET` environment variable chooses another minimum.

Results are identical across kernels on the same platform (see
[architecture.md](architecture.md#numerics-and-exactness)).

## The training stack

Training AI Specialists needs Python 3.10 or newer (3.14, or the `zstd` command, to unpack the binary exams) and
PyTorch 2.2 or newer, on a CPU or a GPU: AMD Radeon GPUs are tested on Windows, and NVIDIA, Linux ROCm and Apple silicon
GPUs use the same stock PyTorch code but have not been run by the maintainers yet. Today the training stack builds
span-finding specialists. It has its own repository, [purebyte-train](https://github.com/purebyte-ai/purebyte-train),
and is installed separately from the runtime: [docs/training.md](https://github.com/purebyte-ai/purebyte-train/blob/main/docs/training.md),
and [Training on any hardware](https://github.com/purebyte-ai/purebyte-train/blob/main/training/docs/gpu.md) for the
PyTorch build that fits your machine.

## Uninstall

Remove the `purebyte` binary (or `pip uninstall purebyte`) and the models directory.
