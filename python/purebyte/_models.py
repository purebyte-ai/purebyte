"""Installed specialists: the catalog (models/models.json, packaged with the library) and the models directory.

The rules are the CLI's (cli/catalog.cpp, cli/engine.cpp), so that `purebyte models pull NAME` installs what
`purebyte.scan(..., model=NAME)` loads: the directory is PUREBYTE_HOME, else the per-user data directory; a model file
is installed under its catalog `file` name, which must be a plain file name; PUREBYTE_CATALOG may point to another
catalog; a name is always a catalog name, and a model file is given by its path.
"""
import hashlib
import json
import os
import sys
from typing import List, NamedTuple, Tuple

from ._errors import ModelFileError, ModelNotInstalledError, PureByteError, UnknownModelError

_HERE = os.path.dirname(os.path.abspath(__file__))
_NAME_CHARACTERS = frozenset("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")


class Specialist(NamedTuple):
    name: str
    version: str
    profile: str  # the catalog's profile hint ("" = the one the model file declares)
    files: List[Tuple[str, bytes]]  # (path, content), checked against the catalog; [0] is the main model


def models_directory() -> str:
    """PUREBYTE_HOME, else the per-user data directory. Never a folder relative to the current one: without those
    variables, PureByteError asks for PUREBYTE_HOME."""
    home = os.environ.get("PUREBYTE_HOME")
    if home:
        return home
    if sys.platform == "win32":
        base = os.environ.get("LOCALAPPDATA")
        if base:
            return base + "\\purebyte\\models"
        missing = "LOCALAPPDATA is not set"
    elif sys.platform == "darwin":
        base = os.environ.get("HOME")
        if base:
            return base + "/Library/Application Support/purebyte/models"
        missing = "HOME is not set"
    else:
        data = os.environ.get("XDG_DATA_HOME")
        if data:
            return data + "/purebyte/models"
        base = os.environ.get("HOME")
        if base:
            return base + "/.local/share/purebyte/models"
        missing = "neither XDG_DATA_HOME nor HOME is set"
    raise PureByteError("io", f"no models directory: {missing}; set PUREBYTE_HOME to the folder of the models")


def _plain_file_name(name: object) -> bool:
    """A catalog `file` is read (and written by `purebyte models pull`) inside the models directory: one plain name
    ending in .gguf, never a path, `..` or a drive."""
    return (isinstance(name, str) and 6 <= len(name) <= 200 and not name.startswith(".") and name.endswith(".gguf")
            and set(name) <= _NAME_CHARACTERS)


def catalog() -> dict:
    """The catalog: PUREBYTE_CATALOG, else the copy packaged with the library (or, in a source tree, models/)."""
    path = os.environ.get("PUREBYTE_CATALOG")
    if not path:
        path = os.path.join(_HERE, "_data", "models.json")
        if not os.path.isfile(path):
            path = os.path.join(_HERE, os.pardir, os.pardir, "models", "models.json")
    try:
        with open(path, encoding="utf-8") as fh:
            data = json.load(fh)
    except OSError as e:
        raise PureByteError("io", f"cannot read the model catalog {path}: {e.strerror}") from None
    except ValueError as e:
        raise ModelFileError(f"the model catalog {path} is not valid JSON: {e}") from None
    if not isinstance(data, dict) or data.get("schema") != 1:
        raise ModelFileError(f"the model catalog {path} has an unknown schema")
    entries = data.get("models", [])
    if not isinstance(entries, list) or not all(isinstance(e, dict) and isinstance(e.get("files", []), list) and
                                                all(isinstance(f, dict) for f in e.get("files", [])) for e in entries):
        raise ModelFileError(f"the model catalog {path} does not follow its schema")
    for entry in entries:
        for f in entry.get("files", []):
            if not _plain_file_name(f.get("file")):
                raise ModelFileError(f"the model catalog {path} gives `{entry.get('name')}` the file "
                                     f"`{str(f.get('file'))[:200]}`: a catalog file must be a plain name ending in "
                                     ".gguf (letters, digits, '.', '_' and '-'), never a path")
    return data


def _version_key(version: str):
    return [int(p) if p.isdigit() else 0 for p in version.split(".")]


def _needs_newer_runtime(entry: dict, runtime: str) -> bool:
    minimum = entry.get("min_runtime") or ""
    return bool(minimum) and _version_key(runtime) < _version_key(minimum)


def resolve(spec: str, runtime: str, ensemble: bool = True) -> Specialist:
    """`name` or `name@version` of an installed specialist -> its files, read once and checked against the catalog's
    SHA-256 (the bytes checked are the bytes the caller loads). Without a version, the highest one that `runtime` (the
    library's version) can run."""
    name, _, version = spec.partition("@")
    entries = [e for e in catalog().get("models", [])
               if e.get("name") == name and (not version or e.get("version") == version)]
    if not entries:
        raise UnknownModelError(f"unknown model `{spec}`: give an installed specialist (see `purebyte models list`), "
                                "or a model file by its path (a name ending in .gguf, or one with a folder such as "
                                "./my-model)")
    runnable = [e for e in entries if not _needs_newer_runtime(e, runtime)]
    entry = max(runnable or entries, key=lambda e: _version_key(e.get("version", "")))
    if _needs_newer_runtime(entry, runtime):
        raise ModelFileError(f"`{entry['name']}` {entry.get('version', '')} needs purebyte {entry['min_runtime']} or "
                             f"newer, and this is purebyte {runtime}: upgrade purebyte", "unsupported")
    directory, files = models_directory(), []
    for f in entry.get("files", []):
        role = f.get("role")
        if role != "model" and (role != "ensemble" or not ensemble):
            continue
        path = os.path.join(directory, f["file"])
        try:
            with open(path, "rb") as fh:
                data = fh.read()
        except FileNotFoundError:
            raise ModelNotInstalledError(f"`{entry['name']}` {entry.get('version', '')} is not installed (missing "
                                         f"{path}): run `purebyte models pull {entry['name']}`") from None
        except OSError as e:
            raise PureByteError("io", f"cannot read the model file {path}: {e.strerror}") from None
        published = f.get("sha256")
        if published and published != "TBD" and hashlib.sha256(data).hexdigest() != published:
            raise ModelFileError(f"{path} does not match its published SHA-256: it is corrupt or was replaced. Run "
                                 "`purebyte models pull` again")
        if role == "model":
            files.insert(0, (path, data))
        else:
            files.append((path, data))
    if not files:
        raise ModelFileError(f"the catalog entry of `{entry['name']}` lists no model file")
    return Specialist(entry["name"], entry.get("version", ""), entry.get("profile", ""), files)


def looks_like_path(spec: str) -> bool:
    """A model file rather than a specialist's name (the CLI's rule for --model): a path with a folder in it, a name
    that starts with `.`, or one that ends in `.gguf`. Anything else is a catalog name, even when a file of that name
    is in the current folder, which may be a repository being scanned."""
    return "/" in spec or "\\" in spec or spec.startswith(".") or spec.endswith(".gguf")
