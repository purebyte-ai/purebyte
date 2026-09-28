#!/usr/bin/env python3
"""Helpers of the release workflow (.github/workflows/release.yml). The procedure is in docs/releasing.md.

Standard library only, Python 3.8 or newer, the same on Linux, macOS and Windows:

    release.py version [--tag vX.Y.Z]
        The runtime version. CMakeLists.txt, python/pyproject.toml and python/purebyte/__init__.py must agree, and
        match the tag when one is given. Examples in the documentation that pin another version are warnings.
    release.py notes VERSION [--or-unreleased]
        The release notes: the section of VERSION in CHANGELOG.md (or the Unreleased one, for a dry run).
    release.py newest VERSION < TAGS
        Prints true when VERSION is at least every vX.Y.Z tag read from standard input, else false.
    release.py published
        Prints NAME@VERSION for every specialist of models/models.json whose files all have a URL and a checksum.
    release.py package --build DIR --target TARGET --version VERSION --out DIR
        The release archive of TARGET from a CMake build directory, and its .sha256 file. TARGET is one of
        linux-x86_64, linux-arm64, macos-arm64, macos-x86_64, windows-x86_64.
    release.py test-wheel WHEEL [--version VERSION]
        Installs WHEEL in a new virtual environment and runs `purebyte version` and python/tests against it.
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
import re
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import time
import venv
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SEMVER = re.compile(r"(\d+)\.(\d+)\.(\d+)")

# The version of the runtime, in every file that states it.
VERSION_SOURCES = [
    ("CMakeLists.txt", r"^project\(purebyte VERSION (\S+?)[\s)]"),
    ("python/pyproject.toml", r'^version = "([^"]*)"'),
    ("python/purebyte/__init__.py", r'^__version__ = "([^"]*)"'),
]

# Examples that users copy, which pin the version of the runtime (docs/releasing.md, "Release a new version").
PINNED_EXAMPLES = [
    ("docs/install.md", "version={v}"),
    ("docs/cli.md", "purebyte {v}"),
    ("integrations/docker/README.md", "purebyte:{v}"),
    ("integrations/gitlab-ci/purebyte.gitlab-ci.yml", "purebyte:{v}"),
    ("integrations/gitlab-ci/README.md", "/v{v}/"),
    ("integrations/pre-commit/README.md", "rev: v{v}"),
    ("README.md", "rev: v{v}"),
]

# Release archives, named as docs/install.md and integrations/github-action/install.sh expect:
# purebyte-<version>-<target>.<extension>, with the executable at the top level.
TARGETS = {
    "linux-x86_64": ("tar.gz", "purebyte", "elf", "x86_64"),
    "linux-arm64": ("tar.gz", "purebyte", "elf", "arm64"),
    "macos-arm64": ("tar.gz", "purebyte", "mach-o", "arm64"),
    "macos-x86_64": ("tar.gz", "purebyte", "mach-o", "x86_64"),
    "windows-x86_64": ("zip", "purebyte.exe", "pe", "x86_64"),
}

# Run-time libraries that a release binary must carry inside instead of importing (PUREBYTE_STATIC_RUNTIME).
RUNTIME_DLLS = ("vcruntime", "msvcp", "api-ms-win-crt-", "ucrtbase", "concrt", "vcomp", "libstdc++", "libgcc",
                "libwinpthread")
MACOS_SYSTEM_LIBRARIES = ("/usr/lib/", "/System/Library/")


class ReleaseError(Exception):
    pass


def annotate(kind: str, message: str, file: str | None = None) -> None:
    """A GitHub Actions annotation (a plain line elsewhere), on standard error so that it never mixes with results."""
    if os.environ.get("GITHUB_ACTIONS") == "true":
        text = message.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
        where = f" file={file}" if file else ""
        print(f"::{kind}{where}::{text}", file=sys.stderr)
    else:
        print(f"{kind}: {file + ': ' if file else ''}{message}", file=sys.stderr)


def read_text(relative: str) -> str:
    with open(os.path.join(ROOT, relative), encoding="utf-8") as f:
        return f.read()


def semver(text: str) -> tuple:
    match = SEMVER.fullmatch(text)
    if not match:
        raise ReleaseError(f"{text!r} is not a version of the form MAJOR.MINOR.PATCH")
    return tuple(int(x) for x in match.groups())


def sha256_of(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


# ------------------------------------------------------------------------------------------------ version and notes


def runtime_version(tag: str | None) -> str:
    found = {}
    for relative, pattern in VERSION_SOURCES:
        match = re.search(pattern, read_text(relative), re.MULTILINE)
        if not match:
            raise ReleaseError(f"{relative}: no version found")
        found[relative] = match.group(1)
    if len(set(found.values())) != 1:
        listed = ", ".join(f"{relative} says {value}" for relative, value in found.items())
        raise ReleaseError(f"the version differs between files ({listed}): set the same MAJOR.MINOR.PATCH in all")
    version = found[VERSION_SOURCES[0][0]]
    semver(version)
    if tag is not None and tag != "v" + version:
        raise ReleaseError(f"the tag {tag} does not match the version {version} of {', '.join(found)}: "
                           f"bump the version first, or tag v{version} (docs/releasing.md)")
    for relative, template in PINNED_EXAMPLES:
        if template.format(v=version) not in read_text(relative):
            annotate("warning", f"no `{template.format(v=version)}` here: update the pinned version of its examples "
                                f"(docs/releasing.md)", relative)
    return version


def changelog_section(text: str, heading: str) -> str | None:
    """The body of the first `## ` section whose heading matches, up to the next section or link definitions."""
    body = None
    for line in text.splitlines():
        if body is None:
            if re.match(heading, line, re.IGNORECASE):
                body = []
        elif line.startswith("## ") or re.match(r"\[[^\]]+\]:\s", line):
            break
        else:
            body.append(line)
    return None if body is None else "\n".join(body).strip()


def release_notes(version: str, or_unreleased: bool) -> str:
    text = read_text("CHANGELOG.md")
    body = changelog_section(text, r"## \[?" + re.escape(version) + r"\]?(\s|$)")
    if body is None and or_unreleased:
        body = changelog_section(text, r"## \[?Unreleased\]?(\s|$)")
        if body is not None:
            annotate("warning", f"no section for {version}: these notes come from [Unreleased] (dry run only)",
                     "CHANGELOG.md")
    if not body:
        raise ReleaseError(f"CHANGELOG.md has no section for {version}, or it is empty: rename [Unreleased] to "
                           f"[{version}] - YYYY-MM-DD before tagging (docs/releasing.md)")
    repository = os.environ.get("GITHUB_REPOSITORY", "purebyte-ai/purebyte")
    return (f"{body}\n\n---\n\nInstall and verify: [docs/install.md](https://github.com/{repository}/blob/v{version}/"
            f"docs/install.md). Every archive below has a `.sha256` checksum file next to it. Docker: "
            f"`ghcr.io/{repository.lower()}:{version}`. Python: `pip install purebyte=={version}`.\n")


def is_newest(version: str, tags: list) -> bool:
    mine = semver(version)
    for tag in tags:
        match = re.search(r"(?:^|/)v(\d+\.\d+\.\d+)$", tag.strip())
        if match and semver(match.group(1)) > mine:
            return False
    return True


def published_specialists() -> list:
    """NAME@VERSION of the catalog entries that `purebyte models pull` accepts (no TBD field)."""
    catalog = json.loads(read_text("models/models.json"))
    return [f"{entry['name']}@{entry['version']}" for entry in catalog["models"]
            if all(f.get("url") not in (None, "TBD") and f.get("sha256") not in (None, "TBD") for f in entry["files"])]


# ----------------------------------------------------------------------------------------------------- binaries


def inspect_binary(path: str) -> tuple:
    """(format, architecture, run-time dependencies) of an executable or library.

    The dependencies are what the system must provide at run time: the dynamic loader for ELF (a static executable
    has none), the imported DLLs for PE, the linked libraries for Mach-O."""
    with open(path, "rb") as f:
        data = f.read()
    try:
        if data[:4] == b"\x7fELF":
            return ("elf",) + _inspect_elf(data)
        if data[:2] == b"MZ":
            return ("pe",) + _inspect_pe(data)
        if data[:4] == b"\xcf\xfa\xed\xfe":
            return ("mach-o",) + _inspect_macho(data)
        if data[:4] == b"\xca\xfe\xba\xbe":
            return ("mach-o", "universal", [])
    except (struct.error, ValueError, KeyError, IndexError) as error:
        raise ReleaseError(f"{path}: malformed executable ({error})") from None
    raise ReleaseError(f"{path}: not an ELF, PE or Mach-O file")


def _inspect_elf(data: bytes) -> tuple:
    endian = {1: "<", 2: ">"}[data[5]]
    machine = struct.unpack_from(endian + "H", data, 18)[0]
    if data[4] == 2:  # 64-bit
        phoff = struct.unpack_from(endian + "Q", data, 32)[0]
        phentsize, phnum = struct.unpack_from(endian + "HH", data, 54)
    else:
        phoff = struct.unpack_from(endian + "I", data, 28)[0]
        phentsize, phnum = struct.unpack_from(endian + "HH", data, 42)
    types = [struct.unpack_from(endian + "I", data, phoff + i * phentsize)[0] for i in range(phnum)]
    needs = ["the dynamic loader (PT_INTERP)"] if 3 in types else []
    return {62: "x86_64", 183: "arm64"}.get(machine, f"machine {machine}"), needs


def _inspect_pe(data: bytes) -> tuple:
    header = struct.unpack_from("<I", data, 0x3C)[0]
    if data[header:header + 4] != b"PE\0\0":
        raise ValueError("no PE signature")
    machine, sections = struct.unpack_from("<HH", data, header + 4)
    optional_size = struct.unpack_from("<H", data, header + 20)[0]
    optional = header + 24
    directories = optional + (112 if struct.unpack_from("<H", data, optional)[0] == 0x20B else 96)
    import_rva = struct.unpack_from("<I", data, directories + 8)[0]  # directory 1: the import table
    table = [struct.unpack_from("<8sIIII", data, optional + optional_size + 40 * i) for i in range(sections)]

    def offset(rva: int) -> int:
        for _, virtual_size, address, raw_size, raw_offset in table:
            if address <= rva < address + max(virtual_size, raw_size):
                return rva - address + raw_offset
        raise ValueError(f"address {rva:#x} is in no section")

    names = []
    position = offset(import_rva) if import_rva else None
    while position is not None:
        descriptor = struct.unpack_from("<IIIII", data, position)
        if not any(descriptor):
            break
        start = offset(descriptor[3])
        names.append(data[start:data.index(b"\0", start)].decode("ascii"))
        position += 20
    return {0x8664: "x86_64", 0xAA64: "arm64", 0x14C: "x86"}.get(machine, f"machine {machine:#x}"), names


def _inspect_macho(data: bytes) -> tuple:
    cputype, _, _, commands = struct.unpack_from("<iiII", data, 4)
    position, libraries = 32, []
    for _ in range(commands):
        command, size = struct.unpack_from("<II", data, position)
        if command in (0xC, 0x80000018, 0x8000001F):  # LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB, LC_REEXPORT_DYLIB
            name = position + struct.unpack_from("<I", data, position + 8)[0]
            libraries.append(data[name:data.index(b"\0", name)].decode("utf-8"))
        position += size
    return {0x01000007: "x86_64", 0x0100000C: "arm64"}.get(cputype, f"cputype {cputype:#x}"), libraries


def check_binary(path: str, kind: str, arch: str, static: bool) -> None:
    """Refuses a binary of another format or architecture, or one that needs a run-time library it should carry."""
    found_kind, found_arch, needs = inspect_binary(path)
    name = os.path.basename(path)
    if (found_kind, found_arch) != (kind, arch):
        raise ReleaseError(f"{name} is a {found_kind} {found_arch} binary, expected {kind} {arch}")
    if kind == "elf" and static and needs:
        problems = needs
    elif kind == "pe":
        problems = [dll for dll in needs if dll.lower().startswith(RUNTIME_DLLS)]
    elif kind == "mach-o":
        problems = [lib for lib in needs if not lib.startswith(MACOS_SYSTEM_LIBRARIES)]
    else:
        problems = []
    if problems:
        raise ReleaseError(f"{name} needs {', '.join(problems)} at run time: release binaries carry their C and C++ "
                           f"runtimes (configure with -DPUREBYTE_STATIC_RUNTIME=ON) and use only system libraries")


# ------------------------------------------------------------------------------------------------------ archives


def find_cli(build: str, name: str) -> str:
    """The `purebyte` executable of a CMake build directory (single- or multi-configuration generator)."""
    found = []
    for directory, subdirectories, files in os.walk(os.path.join(build, "cli")):
        subdirectories[:] = [d for d in subdirectories if d != "CMakeFiles"]
        found += [os.path.join(directory, name) for f in files if f == name]
    release = [path for path in found if os.path.basename(os.path.dirname(path)) == "Release"]
    if len(found) == 1 or len(release) == 1:
        return (release or found)[0]
    raise ReleaseError(f"expected one {name} under {os.path.join(build, 'cli')}, found {len(found)}: "
                       f"{', '.join(found) or 'none'} (build the Release configuration first)")


def archive_members(cli: str, name: str) -> list:
    """What an archive holds, flat: the executable, the license, and the license of every vendored library."""
    members = [(name, cli, 0o755), ("LICENSE", os.path.join(ROOT, "LICENSE"), 0o644)]
    third_party = os.path.join(ROOT, "cli", "third_party")
    for library in sorted(os.listdir(third_party)):
        license_file = os.path.join(third_party, library, "LICENSE")
        if os.path.isfile(license_file):
            members.append((f"LICENSE-{library}", license_file, 0o644))
    return members


def source_date_epoch() -> int:
    """The time stamp of every member: SOURCE_DATE_EPOCH (the workflow sets the commit time), else now."""
    return int(os.environ.get("SOURCE_DATE_EPOCH") or time.time())


def write_tar_gz(path: str, members: list, epoch: int) -> None:
    with open(path, "wb") as raw, gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=epoch) as compressed:
        with tarfile.open(fileobj=compressed, mode="w", format=tarfile.USTAR_FORMAT) as tar:
            for name, source, mode in members:
                info = tarfile.TarInfo(name)
                info.size, info.mtime, info.mode = os.path.getsize(source), epoch, mode
                info.uid, info.gid, info.uname, info.gname = 0, 0, "root", "root"
                with open(source, "rb") as f:
                    tar.addfile(info, f)


def write_zip(path: str, members: list, epoch: int) -> None:
    stamp = time.gmtime(max(epoch, 315532800))[:6]  # zip dates start in 1980
    with zipfile.ZipFile(path, "w") as archive:
        for name, source, mode in members:
            info = zipfile.ZipInfo(name, date_time=stamp)
            info.create_system, info.external_attr = 3, (stat.S_IFREG | mode) << 16
            with open(source, "rb") as f:
                archive.writestr(info, f.read(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)


def archive_contents(path: str) -> dict:
    """{name: sha256} of the regular files of an archive."""
    contents = {}
    if path.endswith(".zip"):
        with zipfile.ZipFile(path) as archive:
            for info in archive.infolist():
                contents[info.filename] = hashlib.sha256(archive.read(info)).hexdigest()
    else:
        with tarfile.open(path, "r:gz") as archive:
            for info in archive.getmembers():
                if info.isfile():
                    contents[info.name] = hashlib.sha256(archive.extractfile(info).read()).hexdigest()
    return contents


def package(build: str, target: str, version: str, out: str) -> str:
    extension, name, kind, arch = TARGETS[target]
    semver(version)
    cli = find_cli(build, name)
    check_binary(cli, kind, arch, static=True)
    members = archive_members(cli, name)
    asset = f"purebyte-{version}-{target}.{extension}"
    os.makedirs(out, exist_ok=True)
    path = os.path.join(out, asset)
    (write_zip if extension == "zip" else write_tar_gz)(path, members, source_date_epoch())
    expected = {member: sha256_of(source) for member, source, _ in members}
    if archive_contents(path) != expected:
        raise ReleaseError(f"{asset} does not hold exactly {', '.join(expected)}")
    digest = sha256_of(path)
    with open(path + ".sha256", "wb") as f:  # the format of sha256sum, LF line ending on every platform
        f.write(f"{digest}  {asset}\n".encode("ascii"))
    print(f"{asset}: {os.path.getsize(path)} bytes, sha256 {digest}")
    print(f"  holds {', '.join(expected)} (from {cli})")
    return path


# -------------------------------------------------------------------------------------------------------- wheels


def run(command: list, **kwargs) -> subprocess.CompletedProcess:
    print("+ " + " ".join(command), flush=True)
    return subprocess.run(command, check=True, **kwargs)


def test_wheel(wheel: str, version: str | None) -> None:
    """Installs the wheel in a fresh virtual environment, outside the source tree, and tests what users get."""
    wheel = os.path.abspath(wheel)
    match = re.match(r"purebyte-(\d+\.\d+\.\d+)-py3-none-(.+)\.whl$", os.path.basename(wheel))
    if not match:
        raise ReleaseError(f"{os.path.basename(wheel)} is not named purebyte-X.Y.Z-py3-none-PLATFORM.whl")
    version = version or match.group(1)
    if match.group(1) != version:
        raise ReleaseError(f"{os.path.basename(wheel)} is not version {version}")
    tests = os.path.join(ROOT, "python", "tests")
    env = {k: v for k, v in os.environ.items() if k not in ("PUREBYTE_LIBRARY", "PUREBYTE_CLI", "PYTHONPATH")}
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    with tempfile.TemporaryDirectory(prefix="purebyte-wheel-") as temp:
        environment = os.path.join(temp, "venv")
        venv.create(environment, with_pip=True)
        scripts = os.path.join(environment, "Scripts" if os.name == "nt" else "bin")
        python = os.path.join(scripts, "python.exe" if os.name == "nt" else "python")
        run([python, "-m", "pip", "install", "--quiet", "--no-index", "--no-deps", wheel], cwd=temp, env=env)

        command = os.path.join(scripts, "purebyte.exe" if os.name == "nt" else "purebyte")
        said = run([command, "version"], cwd=temp, env=env, capture_output=True, text=True).stdout.strip()
        if said != f"purebyte {version}":
            raise ReleaseError(f"`purebyte version` of the wheel prints {said!r}, expected 'purebyte {version}'")
        probe = "import os, purebyte; print(os.path.dirname(purebyte.__file__)); print(purebyte.__version__); " \
                "print(purebyte.version())"
        lines = run([python, "-c", probe], cwd=temp, env=env, capture_output=True, text=True).stdout.split()
        package_dir, module_version, library_version = lines[0], lines[1], lines[2]
        if not os.path.realpath(package_dir).startswith(os.path.realpath(environment)):
            raise ReleaseError(f"the test imported purebyte from {package_dir}, not from the wheel")
        if (module_version, library_version) != (version, version):
            raise ReleaseError(f"purebyte.__version__ is {module_version} and purebyte.version() {library_version}, "
                               f"expected {version}")
        platform = match.group(2)
        arch = "arm64" if platform.endswith(("arm64", "aarch64")) else "x86_64"
        kind = "pe" if platform.startswith("win") else "mach-o" if platform.startswith("macosx") else None
        if kind:  # Linux wheels are checked by auditwheel, which also sets their manylinux tag
            for native in sorted(os.listdir(os.path.join(package_dir, "_lib"))):
                if not native.startswith("LICENSE"):
                    check_binary(os.path.join(package_dir, "_lib", native), kind, arch, static=True)
        run([python, "-m", "unittest", "discover", "-s", tests, "-t", tests], cwd=temp, env=env)
    print(f"{os.path.basename(wheel)}: installed and tested")


# ---------------------------------------------------------------------------------------------------------- main


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    p = commands.add_parser("version", help="print the runtime version after checking that every file agrees")
    p.add_argument("--tag", help="the tag being released, which must be v<version>")
    p = commands.add_parser("notes", help="print the release notes of VERSION from CHANGELOG.md")
    p.add_argument("version")
    p.add_argument("--or-unreleased", action="store_true", help="use the Unreleased section if VERSION has none")
    p = commands.add_parser("newest", help="print true if VERSION is at least every vX.Y.Z tag of standard input")
    p.add_argument("version")
    commands.add_parser("published", help="print NAME@VERSION of every specialist of the catalog that can be pulled")
    p = commands.add_parser("package", help="write the release archive of a target and its .sha256 file")
    p.add_argument("--build", required=True, help="the CMake build directory")
    p.add_argument("--target", required=True, choices=sorted(TARGETS))
    p.add_argument("--version", required=True)
    p.add_argument("--out", required=True, help="the directory to write to")
    p = commands.add_parser("test-wheel", help="install a wheel in a new virtual environment and test it")
    p.add_argument("wheel")
    p.add_argument("--version", help="the expected version (default: the one in the wheel's name)")
    args = parser.parse_args(argv)
    try:
        if args.command == "version":
            print(runtime_version(args.tag))
        elif args.command == "notes":
            sys.stdout.write(release_notes(args.version, args.or_unreleased))
        elif args.command == "newest":
            print("true" if is_newest(args.version, sys.stdin.read().splitlines()) else "false")
        elif args.command == "published":
            print("\n".join(published_specialists()))
        elif args.command == "package":
            package(args.build, args.target, args.version, args.out)
        elif args.command == "test-wheel":
            test_wheel(args.wheel, args.version)
    except ReleaseError as error:
        annotate("error", str(error))
        return 1
    except subprocess.CalledProcessError as error:
        annotate("error", f"{' '.join(map(str, error.cmd))} failed with exit status {error.returncode}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
