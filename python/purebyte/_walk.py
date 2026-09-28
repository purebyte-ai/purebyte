"""What `purebyte.scan` reads from files and directories, with the rules of the CLI (cli/inputs.cpp, cli/exclusions.h):
the profile chooses the files and folders a walk enters; links are never followed (symbolic links, and on Windows
junctions and the other reparse points that stand for another file or folder); `exclude` patterns and the lines of a
folder's .purebyteignore leave paths out; files named explicitly are always read; empty files are skipped; files over
the profile's limit are failures, found before they are read.
"""
import os
import stat
from typing import List, NamedTuple, Optional, Sequence, Tuple, Union

from ._errors import ArgumentError, PureByteError

_REPARSE_POINT = 0x400  # FILE_ATTRIBUTE_REPARSE_POINT
_NAME_SURROGATE = 0x20000000  # IsReparseTagNameSurrogate: a junction, a symbolic link, a WSL link


def _encode(text: str) -> bytes:
    return text.encode("utf-8", "surrogateescape")


def _segments(path: str) -> List[bytes]:
    return [_encode(p) for p in path.replace("\\", "/").split("/") if p and p != "."]


def _glob_match(pattern: bytes, name: bytes) -> bool:
    """`*` and `?` against one path segment, byte by byte, as the CLI matches."""
    p = n = 0
    star, resume = -1, 0
    while n < len(name):
        if p < len(pattern) and pattern[p] in (0x3F, name[n]):  # '?' or the same byte
            p, n = p + 1, n + 1
        elif p < len(pattern) and pattern[p] == 0x2A:  # '*'
            star, resume, p = p, n, p + 1
        elif star != -1:
            resume += 1
            p, n = star + 1, resume
        else:
            return False
    while p < len(pattern) and pattern[p] == 0x2A:
        p += 1
    return p == len(pattern)


def _prefix_matches(parts: List[bytes], s: List[bytes]) -> List[bool]:
    """hits[n]: the pattern's segments match the path's first n segments, `**` taking any number of them (dynamic
    programming, as the CLI: the work does not grow exponentially with the number of `**`)."""
    row = [True] + [False] * len(s)
    for part in parts:
        if part == b"**":
            reached, nxt = False, []
            for j in range(len(s) + 1):
                reached = reached or row[j]
                nxt.append(reached)
        else:
            nxt = [False] * (len(s) + 1)
            for j in range(len(s)):
                if row[j] and _glob_match(part, s[j]):
                    nxt[j + 1] = True
        row = nxt
    return row


class Exclusions:
    """Paths left out of a walk, in the syntax of .gitignore that the CLI accepts (docs/cli.md, "Ignoring findings"):
    a pattern without `/` matches a name at any depth, one with `/` the path from the top of the walk; `**` stands for
    any number of folders; a trailing `/` matches folders only; negation and character classes are refused."""

    class _Pattern(NamedTuple):
        parts: List[bytes]
        fixed: int  # segments other than `**`
        anchored: bool
        directory_only: bool

    def __init__(self, patterns: Sequence[str] = ()):
        self._patterns: List[Exclusions._Pattern] = []
        for pattern in patterns:
            self.add(pattern)

    def add(self, raw: str) -> None:
        text = raw.strip(" \t\r\n")
        if not text:
            raise ArgumentError("an exclusion pattern cannot be empty")
        if text.startswith("!"):
            raise ArgumentError(f"`{text}`: negated exclusion patterns (!) are not supported")
        if "[" in text:
            raise ArgumentError(f"`{text}`: character classes ([...]) are not supported in exclusion patterns")
        directory_only = text[-1] in "/\\"
        if directory_only:
            text = text[:-1]
        leading = text[:1] in ("/", "\\")
        parts: List[bytes] = []
        for part in _segments(text):  # `a/**/**/b` is `a/**/b`
            if part != b"**" or not parts or parts[-1] != b"**":
                parts.append(part)
        if not parts:
            raise ArgumentError(f"`{raw}`: an exclusion pattern must name something")
        self._patterns.append(Exclusions._Pattern(parts, sum(p != b"**" for p in parts), leading or len(parts) > 1,
                                                  directory_only))

    def load(self, path: str) -> None:
        """The patterns of a .purebyteignore file: a missing file adds nothing; a link is not followed."""
        if os.path.islink(path):
            raise PureByteError("io", f"the exclusion file {path} is a symbolic link, which is not followed")
        if not os.path.exists(path):
            return
        try:
            with open(path, "rb") as fh:
                text = fh.read().decode("utf-8", "surrogateescape")
        except OSError as e:
            raise PureByteError("io", f"cannot read the exclusion file {path}: {e.strerror}") from None
        if text.startswith("﻿"):
            text = text[1:]
        for line in text.split("\n"):
            line = line.strip(" \t\r\n")
            if line and not line.startswith("#"):
                self.add(line)

    def excluded(self, path: str, directory: bool) -> bool:
        """`path`: from the top of the walk, `/` or `\\` separated. A folder that matches leaves out all below it."""
        s = _segments(path)
        for pattern in self._patterns:
            if not pattern.anchored:
                hits = [False] + [_glob_match(pattern.parts[0], segment) for segment in s]
            elif pattern.fixed > len(s):
                continue
            else:
                hits = _prefix_matches(pattern.parts, s)
            for length in range(1, len(s) + 1):
                folder = length < len(s) or directory
                if hits[length] and (folder or not pattern.directory_only):
                    return True
        return False


def is_link(path: str) -> bool:
    """A link that a walk does not follow: a symbolic link, or on Windows a reparse point that stands for another file
    or folder (a junction, which os.path.islink does not report, would make a walk endless when it points above)."""
    try:
        st = os.lstat(path)
    except OSError:
        return False
    if stat.S_ISLNK(st.st_mode):
        return True
    if getattr(st, "st_file_attributes", 0) & _REPARSE_POINT:
        return bool(getattr(st, "st_reparse_tag", _NAME_SURROGATE) & _NAME_SURROGATE)
    return False


class Item(NamedTuple):
    """One input to scan: its display name, where its bytes are (a path, or the bytes themselves), its path inside
    the project for the profile's path rules (None = its name), and its size."""
    name: str
    source: Union[str, bytes]
    rule: Optional[str]
    size: int


def display(path: str) -> str:
    """A path as reports show it, as the CLI writes it: `/` as the separator on every platform."""
    return path.replace(os.sep, "/") if os.sep != "/" else path


def _join(parent: str, name: str) -> str:
    if not parent:
        return name
    return parent + name if parent.endswith("/") else parent + "/" + name


def _outside_project(path: str) -> bool:
    """An absolute path, or one that climbs out of the working directory: the folders it names are not part of the
    project scanned, so the path rules only look below it (as the CLI does)."""
    if os.path.isabs(path) or os.path.splitdrive(path)[0] or path.startswith(("/", os.sep)):
        return True
    return os.path.normpath(path).split(os.sep)[0] == os.pardir


def _walk(detector, directory: str, shown: str, rule: str, top: str, exclusions: Exclusions,
          files: List[Tuple[str, str, str]], failures: List[dict], depth: int = 0) -> None:
    if depth > 128:
        failures.append({"file": shown, "reason": "more than 128 nested folders (a link cycle?): not walked"})
        return
    try:
        entries = list(os.scandir(directory))
    except OSError as e:
        failures.append({"file": shown, "reason": f"could not be listed: {e.strerror}"})
        return
    subdirectories = []
    for entry in entries:
        name, entry_top = entry.name, _join(top, entry.name)
        try:
            if entry.is_dir():
                if (detector.wants_directory(name) and not is_link(entry.path) and
                        not exclusions.excluded(entry_top, True)):
                    subdirectories.append((entry.path, _join(shown, name), _join(rule, name), entry_top))
            elif detector.wants_file(name) and not is_link(entry.path) and not exclusions.excluded(entry_top, False):
                files.append((_join(shown, name), entry.path, _join(rule, name)))
        except OSError:
            continue
    for path, sub_shown, sub_rule, sub_top in subdirectories:
        _walk(detector, path, sub_shown, sub_rule, sub_top, exclusions, files, failures, depth + 1)


def plan(detector, sources: Sequence[Union[str, bytes, bytearray, "os.PathLike[str]"]],
         exclude: Sequence[str] = ()) -> Tuple[List[Item], List[dict]]:
    """The inputs of `sources` (paths and bytes), in the CLI's order, and the ones that cannot be scanned."""
    limit = detector.max_input_bytes
    Exclusions(exclude)  # a bad pattern is refused even when no folder is walked
    items: List[Item] = []
    files: List[Tuple[str, str, str]] = []
    failures: List[dict] = []
    for source in sources:
        if isinstance(source, (bytes, bytearray)):
            items.append(Item("", bytes(source), None, len(source)))
            continue
        path = os.fspath(source)
        outside = _outside_project(path)
        if os.path.isdir(path):
            exclusions = Exclusions(exclude)  # `exclude`, then the folder's .purebyteignore: paths from its top
            exclusions.load(os.path.join(path, ".purebyteignore"))
            _walk(detector, path, display(path), "" if outside else display(path), "", exclusions, files, failures)
        elif os.path.exists(path):  # a file named explicitly is always scanned
            files.append((display(path), path, os.path.basename(path) if outside else display(path)))
        else:
            failures.append({"file": display(path), "reason": "does not exist"})
    for shown, path, rule in sorted(files):
        try:
            size = os.path.getsize(path)
        except OSError as e:
            failures.append({"file": shown, "reason": f"could not be read: {e.strerror}"})
            continue
        if size == 0:
            continue
        if size > limit:
            failures.append({"file": shown, "reason": f"{size} bytes, over this model's limit of {limit} bytes: "
                                                      "not scanned"})
            continue
        items.append(Item(shown, path, rule or None, size))
    return items, failures
