# Releasing

This page is for maintainers. A release of the runtime is one tag, `vX.Y.Z`: the
[release workflow](../.github/workflows/release.yml) builds and tests the runtime on every platform, then publishes
what [install.md](install.md) promises. The weights of the AI Specialists are released separately, by hand, under
their own tags ([Model weights](#model-weights)).

## What a release publishes

| What | Where | Names |
|---|---|---|
| The CLI: one archive per platform, each with a SHA-256 file and a build provenance attestation | The GitHub release `vX.Y.Z` (the attestations are stored with the repository) | `purebyte-X.Y.Z-linux-x86_64.tar.gz`, `-linux-arm64.tar.gz`, `-macos-arm64.tar.gz`, `-macos-x86_64.tar.gz` and `-windows-x86_64.zip`, each with `<archive>.sha256` next to it |
| The Docker image, `linux/amd64` and `linux/arm64` | `ghcr.io/purebyte-ai/purebyte` | Tags `X.Y.Z` and `latest` |
| The Python package: one wheel per platform | PyPI, project `purebyte` | `purebyte-X.Y.Z-py3-none-<platform>.whl` for the same five platforms |
| Model weights | GitHub releases `<name>-<version>`, made by hand | `purebyte-<name>-<version>[-eN].gguf` and `.sha256` |

Each archive holds, at its top level, `purebyte` (`purebyte.exe` on Windows), `LICENSE`, and `LICENSE-cpp-httplib`
for the [vendored HTTP library](../cli/third_party/cpp-httplib/VENDORED.md). A `.sha256` file has the format of
`sha256sum`: `<hex digest>  <archive name>`. These are the names that [install.md](install.md#release-binaries) and
the [GitHub Action](../integrations/github-action/install.sh) use. The release notes are the section of the version in
[CHANGELOG.md](../CHANGELOG.md), followed by where to find the install instructions.

A `.sha256` file next to an archive only detects a corrupt download: whoever could replace the archive could replace
it too. So the archives also get a build provenance attestation (`actions/attest`), signed through Sigstore with the
identity of the release workflow and the tag it ran on, and stored with the repository. `gh attestation verify
<archive> --repo purebyte-ai/purebyte` checks it, and the GitHub Action does before it runs the binary. The wheels
carry PyPI's own attestations of the workflow that uploaded them.

The binaries carry their C and C++ runtimes (`-DPUREBYTE_STATIC_RUNTIME=ON`, with the project's compiler flags, which
protect the numerics): the Linux ones are fully static, the Windows ones (the DLL of the wheel too) need no Visual C++
Redistributable, and the macOS ones use only system libraries and run on macOS 11 or newer. Linux wheels are built for
manylinux_2_28 (glibc 2.28 or newer). There is no source distribution on PyPI: the package builds the C++ runtime from
the whole repository, which `python/` alone does not hold; other platforms build from source
([install.md](install.md#from-source)). The binaries are not signed with a developer certificate, nor notarized for
macOS: a copy downloaded with a web browser may be stopped by Gatekeeper or SmartScreen.

## One-time settings

### GitHub

1. **The organization and the repository**: `purebyte-ai/purebyte`, public. The documentation, the model catalog and
   the image name use it, and the release workflow publishes only from it.
2. **The `pypi` environment**: Settings → Environments → New environment, named `pypi`. Under *Deployment branches and
   tags*, choose *Selected branches and tags* and add the tag rule `v*.*.*`. Optionally add maintainers as *Required
   reviewers*, so that each upload to PyPI waits for an approval. It needs no secret: PyPI trusts the workflow itself.
3. **Actions**: Settings → Actions → General. If the organization allows only selected actions, allow
   `actions/checkout`, `actions/setup-python`, `actions/upload-artifact`, `actions/download-artifact`,
   `actions/attest`, `docker/setup-buildx-action`, `docker/login-action`, `docker/build-push-action`,
   `pypa/cibuildwheel`, `pypa/gh-action-pypi-publish` and `msys2/setup-msys2`. The workflows pin every action to a full commit SHA, so the
   policy that requires it can be turned on. The default permissions of `GITHUB_TOKEN` can stay read-only: each job
   asks for what it needs.
4. **Arm runners**: the Linux arm64 jobs run on `ubuntu-24.04-arm`, which is free for public repositories only. Make
   the repository public before the first dry run, or these jobs wait for a runner that never comes.
5. **The container package, after the first release**: the first push creates `ghcr.io/purebyte-ai/purebyte` as a
   private package. On the package page, Package settings → Change visibility → Public (the organization must allow
   public packages: Organization settings → Packages). If a later release cannot push, give the repository the Write
   role under *Manage Actions access*.
6. **Recommended**: a tag ruleset (Settings → Rules → Rulesets) that lets only maintainers create, move or delete
   `v*` tags; release immutability (Settings → General → Releases), which the workflow supports because it uploads
   every asset before it publishes; and private vulnerability reporting (Settings → Code security), which
   [SECURITY.md](../SECURITY.md) asks reporters to use.
7. **Labels and Discussions**: the issue forms ([.github/ISSUE_TEMPLATE](../.github/ISSUE_TEMPLATE)) apply the labels
   `bug`, `enhancement`, `model-quality` and `triage`, and GitHub leaves out a label that does not exist without a
   word: create `model-quality` and `triage` (Issues → Labels → New label; the other two exist by default). The issue
   chooser links to Discussions: turn them on (Settings → General → Features → Discussions).

### PyPI

1. With the account of a maintainer (two-factor authentication on), add a **pending trusted publisher** at
   <https://pypi.org/manage/account/publishing/>, type GitHub:

   | Field | Value |
   |---|---|
   | PyPI project name | `purebyte` |
   | Owner | `purebyte-ai` |
   | Repository name | `purebyte` |
   | Workflow name | `release.yml` |
   | Environment name | `pypi` |

   The first upload creates the project and makes it a normal trusted publisher. A pending publisher does not reserve
   the name: until the first upload, anyone can register `purebyte`.
2. After the first release, add a second maintainer to the project.

No API token is created or stored anywhere: the `pypi` job gets a short-lived one from PyPI through OpenID Connect
(`id-token: write`), and each file is uploaded with an attestation of the workflow that built it.

## Release a new version

1. **Prepare it in a pull request.**
   - The version, in the three files that state it: `CMakeLists.txt` (`project(purebyte VERSION X.Y.Z ...)`),
     `python/pyproject.toml` (`version`) and `python/purebyte/__init__.py` (`__version__`). The workflow refuses a tag
     that does not match all three.
   - The examples that pin the version: [install.md](install.md), [cli.md](cli.md), the pre-commit example of the
     main README, and the READMEs of `integrations/docker`, `integrations/gitlab-ci` (and its template) and
     `integrations/pre-commit`
     (`git grep -n 'X\.Y\.Z'` with the previous version). The workflow warns about the ones it knows.
   - `CHANGELOG.md`: rename `## [Unreleased]` to `## [X.Y.Z] - YYYY-MM-DD` and open a new, empty `## [Unreleased]`
     above it. That section becomes the release notes.
   - `models/models.json`, which is compiled into the CLI: every specialist this version offers has its final URL,
     SHA-256 and size, and its weights are released before the tag ([Model weights](#model-weights)).
2. **Dry run**: Actions → release → Run workflow, on `main`. Everything is built and tested, the archives, wheels and
   notes are kept as artifacts of the run, and nothing is published. Check them as in
   [Verify a release](#verify-a-release). Until the model releases exist, the `models pull` job fails; in a dry run
   that is only reported.
3. **Tag** the merged commit and push the tag:

   ```bash
   git switch main && git pull --ff-only
   git tag -a vX.Y.Z -m "PureByte X.Y.Z"    # -s instead of -a signs it
   git push origin vX.Y.Z
   ```

4. **Follow the run** (Actions → release) and approve the `pypi` deployment if the environment asks for a reviewer.
5. **Move the major tag of the GitHub Action**, which users pin as `@v1` (this tag does not start the workflow):

   ```bash
   git tag -f v1 'vX.Y.Z^{}' && git push -f origin v1
   ```

6. **Verify the release** (below), then announce it.

### What the workflow does

| Job | What it does |
|---|---|
| version and notes | Checks that the three versions agree and match the tag, and writes the notes from `CHANGELOG.md` (from `[Unreleased]` in a dry run). It publishes only for a tag pushed to `purebyte-ai/purebyte`, and marks the release and the image `latest` only for the highest `vX.Y.Z` |
| binary (5 platforms) | Configures with `-DPUREBYTE_STATIC_RUNTIME=ON`, builds, runs the whole test suite and packages; the packaging ([release.py](../.github/scripts/release.py)) refuses a binary of another architecture, or one that needs a C or C++ runtime or a non-system library. Then it verifies and unpacks the archive with the commands of `install.md` and runs it. On macOS and Windows it also makes the wheel from the same build and tests it in a new virtual environment |
| wheel (Linux, 2 architectures) | In the manylinux_2_28 image: builds the runtime and runs the test suite, makes the wheel from that build, lets auditwheel check it and set its tag, and tests it in a new virtual environment |
| docker (2 architectures) | Builds `integrations/docker/Dockerfile` natively on each architecture (the build runs the test suite) and smoke-tests the image; when publishing, pushes it by digest, without a tag, and smoke-tests the pushed image again, pulled by its digest |
| models pull | With the Linux x86-64 archive: pulls every specialist of the catalog that has a URL, verifies them and runs the installation check of `install.md` |
| publish the Docker image | Tags the two images as one multi-architecture image, `X.Y.Z` and `latest` |
| publish to PyPI | Uploads the five wheels through trusted publishing |
| publish the GitHub release | Verifies the checksums, attests the provenance of the five archives, and creates the release with its ten files and the notes |

Nothing is published under a version (a release, a tag of the image, a version on PyPI) until every build and test has
passed, and the GitHub release comes last, so that the release page (and `version: latest` of the GitHub Action)
shows only complete releases. The one thing that reaches a registry earlier is the pair of untagged per-architecture
images that the docker jobs push by digest, before the other jobs finish; they get a tag only in the last Docker job.
If a run fails after they are pushed, they stay in the package's list of versions without a tag, and can be deleted
there (package page → Manage versions). If a publishing job fails, for example
because PyPI is not configured yet, fix the cause and choose *Re-run failed jobs*: the jobs that passed are not
repeated, and the artifacts of the run are reused.

### Verify a release

```bash
version=X.Y.Z
gh release download "v$version" --repo purebyte-ai/purebyte --dir "purebyte-$version"
cd "purebyte-$version" && sha256sum -c ./*.sha256    # macOS: shasum -a 256 -c ./*.sha256
for archive in purebyte-*.tar.gz purebyte-*.zip; do  # the provenance: built by release.yml from the tag
  gh attestation verify "$archive" --repo purebyte-ai/purebyte \
    --signer-workflow purebyte-ai/purebyte/.github/workflows/release.yml --source-ref "refs/tags/v$version"
done
```

Then, on the platforms you can reach, install as [install.md](install.md) says and run its installation check. The
image and the package:

```bash
docker buildx imagetools inspect "ghcr.io/purebyte-ai/purebyte:$version"   # linux/amd64 and linux/arm64
docker run --rm "ghcr.io/purebyte-ai/purebyte:$version" version           # purebyte X.Y.Z
python -m venv /tmp/pb && /tmp/pb/bin/pip install "purebyte==$version" && /tmp/pb/bin/purebyte version
```

<https://pypi.org/project/purebyte/#files> lists the five wheels, each with its provenance. In a test repository, a
workflow with `uses: purebyte-ai/purebyte/integrations/github-action@v1` installs the new version; its log names the
archive it downloaded.

## Model weights

The weights of each specialist are released by hand under the tag `<name>-<version>` (`secrets-code-1.0.0`,
`secrets-bin-1.0.0`, `pii-1.0.0`), which is where [models.json](../models/models.json) points:
`https://github.com/purebyte-ai/purebyte/releases/download/<name>-<version>/<file>`. These tags do not start the
release workflow. Weights are never committed.

1. **Make the public bundle** with the training stack in [purebyte-train](https://github.com/purebyte-ai/purebyte-train),
   from a checkout of it: `python -m pbtrain release RECIPE --public --semver X.Y.Z` (RECIPE is the recipe's `.yaml`,
   which the help of `pbtrain` calls TASK) writes
   `$PUREBYTE_WORK/releases/<name>-<version>/` (`$PUREBYTE_WORK` is `~/.purebyte/work` by default): the `.gguf` files
   (`-e1`, `-e2` for ensemble members), one `.sha256` per file, `MODEL_CARD.md` and `models.json.fragment`
   ([its checklist](https://github.com/purebyte-ai/purebyte-train/blob/main/training/docs/release.md)).
2. **Check it against the catalog**: `sha256sum -c ./*.sha256` in the bundle; the `url`, `sha256` and `size` of each
   of its files in `models/models.json` must match the fragment. `secrets-code-ensemble` uses the three files of the
   `secrets-code` bundle.
3. **Check `pull` before uploading**, from the root of this repository, with a `purebyte` built from `main` on the
   `PATH` and a copy of the catalog whose URLs point at the bundles (a local mirror, [install.md](install.md#models)):

   ```bash
   mirror=$(mktemp)
   python - > "$mirror" <<'EOF'
   import json, os, pathlib
   catalog = json.load(open("models/models.json", encoding="utf-8"))
   releases = pathlib.Path(os.environ.get("PUREBYTE_WORK", pathlib.Path.home() / ".purebyte" / "work")) / "releases"
   for entry in catalog["models"]:
       for f in entry["files"]:
           if f["url"] != "TBD":
               tag, name = f["url"].split("/")[-2:]
               f["url"] = (releases / tag / name).as_uri()
   print(json.dumps(catalog, indent=2))
   EOF
   export PUREBYTE_CATALOG="$mirror" PUREBYTE_HOME="$(mktemp -d)"
   for s in secrets-code secrets-code-ensemble secrets-bin pii; do purebyte models pull "$s"; done
   purebyte models verify
   unset PUREBYTE_CATALOG PUREBYTE_HOME
   ```

4. **Tag the commit of `main` whose catalog lists the files, and create the release.** Use `--latest=false`, so that
   the runtime release stays the latest release of the repository:

   ```bash
   tag=secrets-code-1.0.0
   git tag -a "$tag" -m "secrets-code 1.0.0" && git push origin "$tag"
   cd "${PUREBYTE_WORK:-$HOME/.purebyte/work}/releases/$tag"
   gh release create "$tag" --repo purebyte-ai/purebyte --verify-tag --latest=false \
     --title "secrets-code 1.0.0" --notes-file notes.md ./*.gguf ./*.gguf.sha256
   ```

   With files, `gh` creates a draft, uploads them, then publishes it. `notes.md` says in one line what the specialist
   does and links its card and the license of the weights at that tag, for example
   `https://github.com/purebyte-ai/purebyte/blob/secrets-code-1.0.0/models/secrets-code.md` and
   `.../blob/secrets-code-1.0.0/MODEL_LICENSE.md`. A forgotten file can be added with `gh release upload "$tag" FILE`
   unless release immutability is on.
5. **Check `pull` end to end from GitHub**, on a clean models directory:

   ```bash
   export PUREBYTE_HOME="$(mktemp -d)"
   for s in secrets-code secrets-code-ensemble secrets-bin pii; do purebyte models pull "$s"; done
   purebyte models verify && purebyte models list
   ```

   Then run the installation check of [install.md](install.md#check-the-installation). The `models pull` job of the
   release workflow does the same with the Linux archive: once the model releases exist, a dry run confirms it, and a
   tagged run does not publish without it.
6. **Mirror the release on Hugging Face**, in the model repository `purebyte/<name>`: the same `.gguf` and `.sha256`
   files, and a card that links the license of the weights at the tag. [MODEL_LICENSE.md](../MODEL_LICENSE.md) names
   the mirror as an official channel and says that it holds the same files, with the same checksums and that license:
   keep it that way. The GitHub release stays the source that `models.json` points to.

A specialist reaches users with the first runtime release whose catalog lists it, since the catalog is compiled into
the CLI; until then, `PUREBYTE_CATALOG` can point to a newer catalog.

## If something goes wrong

- **A job fails before publishing**: nothing was published. Fix the cause on `main`, delete the tag
  (`git push origin :refs/tags/vX.Y.Z` and `git tag -d vX.Y.Z`) and tag again.
- **A publishing job fails**: fix the cause, then *Re-run failed jobs*.
- **A published release is broken**: publish a patch release. PyPI never accepts a version number twice (yank the
  broken one on PyPI instead), and with release immutability the assets of a GitHub release cannot change.

## Maintaining the workflow

- What is not YAML is in [.github/scripts/release.py](../.github/scripts/release.py) (standard library, Python 3.8 or
  newer), which runs locally too: `python .github/scripts/release.py version`, `... notes X.Y.Z --or-unreleased`,
  `... package --build build --target windows-x86_64 --version X.Y.Z --out dist`, `... test-wheel WHEEL`.
- Actions are pinned to full commit SHAs, with the version in a comment, and Dependabot proposes updates
  ([dependabot.yml](../.github/dependabot.yml)). To pin one by hand, take the SHA of the tag from
  `git ls-remote --tags https://github.com/OWNER/NAME 'refs/tags/vX.Y.Z*'` (the `^{}` line of an annotated tag).
- The release jobs install NumPy with pip, pinned in the workflow's `env`, for the test that compares the engine with
  the NumPy reference; the wheel builds install the build requirements of `python/pyproject.toml`, pinned to one
  version of setuptools that is changed on purpose, in its own pull request (building a wheel needs Python 3.10 or
  newer; the package runs on 3.8 or newer), and cibuildwheel pins its own images and tools.
- The workflow uses no cache, so that nothing from another run can reach a release.
