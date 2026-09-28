"""The Python face of libpurebyte: models, sessions, detectors, and the one-call helpers scan / decide / redact."""
import ctypes
import json
import os
from typing import Dict, Iterable, List, Optional, Sequence, Tuple, Union

from . import _models, _walk
from ._errors import ArgumentError, PureByteError
from ._native import (DETECT_EARLY_EXIT, DETECT_EXPAND_ARCHIVES, DETECT_KEEP_EXAMPLES, DETECT_NO_ENSEMBLE,
                      DETECT_PER_MODEL, DETECT_PREFILTER, DETECT_REVEAL, DETECT_STRINGS_ONLY, REDACT_WITH_MAP,
                      SEVERITY_WARNING, Error, Input, RedactionSpan, archive_limits, detect_options, lib,
                      redact_options, session_options)

PathLike = Union[str, "os.PathLike[str]"]
DECIDE_LIMIT = 4096
BATCH_BYTES = 64 << 20  # inputs handed to the library at once by scan(), as the CLI does


def _check(status: int, err: Error, what: str) -> None:
    if status != 0:
        name = lib.pb_status_name(status).decode()
        raise PureByteError(name, f"{what}: {err.message.decode('utf-8', 'replace')}")


def _encode(text: str) -> bytes:
    """A name or path as the library takes it (UTF-8); undecodable bytes of POSIX names pass through as they are."""
    return os.fsdecode(text).encode("utf-8", "surrogateescape")


def version() -> str:
    """Version of the loaded library, "MAJOR.MINOR.PATCH"."""
    return lib.pb_version().decode()


# ------------------------------------------------------------------------------------------------------ handles


class Model:
    """A loaded model file (GGUF). Immutable; any number of detectors and threads may use it."""

    def __init__(self, path: PathLike):
        self.path = os.fspath(path)
        handle, err = ctypes.c_void_p(), Error()
        _check(lib.pb_model_load(_encode(self.path), ctypes.byref(handle), ctypes.byref(err)), err,
               f"cannot load {self.path}")
        self._handle = handle

    @classmethod
    def _from_memory(cls, data: bytes, path: str) -> "Model":
        """A model loaded from bytes already read (and checked) by the caller; the library copies them."""
        model = cls.__new__(cls)
        model.path = path
        handle, err = ctypes.c_void_p(), Error()
        _check(lib.pb_model_load_memory(data, len(data), ctypes.byref(handle), ctypes.byref(err)), err,
               f"cannot load {path}")
        model._handle = handle
        return model

    def describe(self) -> dict:
        """What the file declares: window, blocks, heads with their names and labels, profile."""
        return json.loads(lib.pb_model_describe(self._handle).decode("utf-8"))

    def close(self) -> None:
        if getattr(self, "_handle", None):
            lib.pb_model_free(self._handle)
            self._handle = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def __del__(self):
        try:
            self.close()
        except Exception:  # at interpreter exit the library may already be gone
            pass


class Session:
    """Worker threads and scratch memory. One call at a time: use one session per thread."""

    def __init__(self, threads: Optional[int] = None, kernel: str = "auto", intra_threads: int = 0):
        options = session_options()
        options.threads = threads if threads is not None else max(1, (os.cpu_count() or 2) - 1)
        options.intra_threads = intra_threads
        options.kernel = kernel.encode()
        handle, err = ctypes.c_void_p(), Error()
        _check(lib.pb_session_create(ctypes.byref(options), ctypes.byref(handle), ctypes.byref(err)), err,
               "cannot start the runtime")
        self._handle = handle

    @property
    def kernel(self) -> str:
        return lib.pb_session_kernel(self._handle).decode()

    @property
    def threads(self) -> int:
        return lib.pb_session_threads(self._handle)

    def close(self) -> None:
        if getattr(self, "_handle", None):
            lib.pb_session_free(self._handle)
            self._handle = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def __del__(self):
        try:
            self.close()
        except Exception:  # at interpreter exit the library may already be gone
            pass


class Finding:
    """One finding of spec/OUTPUT.md. Attributes are its JSON fields; optional ones it lacks are None. `severity` is
    "error", or "warning" for a file in a test, example or documentation path (secrets-code)."""

    FIELDS = ("file", "kind", "severity", "start", "end", "line", "col", "end_line", "confidence", "votes",
              "snippet_masked", "context_masked", "string_masked", "offset_hex", "inside_base64", "inner_start",
              "inner_end", "snippet", "context", "string")
    __slots__ = ("_data",)

    def __init__(self, data: dict):
        self._data = data

    def __getattr__(self, name):
        if name in Finding.FIELDS:
            return self._data.get(name)
        raise AttributeError(name)

    def as_dict(self) -> dict:
        return dict(self._data)

    def __repr__(self):
        where = f"{self.line}:{self.col}" if self.line is not None else f"@{self.start}"
        return f"Finding({self.file!r} {where} {self.kind} {self.snippet_masked!r})"


class ScanResult:
    """What a scan returns: `findings` (flat list), `results` (the generic per-input objects) and `failures`
    (inputs that could not be scanned: {"file", "reason"}). `by_severity` counts the findings: {"error": n,
    "warning": n}; only errors should fail a check (the CLI's exit status)."""

    def __init__(self, document: dict, failures: Optional[List[dict]] = None):
        self.document = document
        self.results: List[dict] = document.get("results", [])
        self.findings = [Finding(f) for f in document.get("findings", [])]
        self.failures: List[dict] = (failures or []) + document.get("failures", [])

    @property
    def by_severity(self) -> Dict[str, int]:
        warnings = sum(1 for f in self.findings if f.severity == "warning")
        return {"error": len(self.findings) - warnings, "warning": warnings}

    def __repr__(self):
        counts = self.by_severity
        return (f"ScanResult({len(self.results)} input(s), {len(self.findings)} finding(s): {counts['error']} "
                f"error(s), {counts['warning']} warning(s), {len(self.failures)} failure(s))")


class Redaction:
    """A redacted copy: `output` (bytes), `report` (dict, never the values) and `map` (dict, only when asked for:
    it holds the values, handle it as a secret)."""

    def __init__(self, handle):
        size = ctypes.c_size_t()
        data = lib.pb_redaction_output(handle, ctypes.byref(size))
        self.output = ctypes.string_at(data, size.value) if size.value else b""
        self.report = json.loads(lib.pb_redaction_report(handle).decode("utf-8"))
        raw_map = lib.pb_redaction_map(handle)
        self.map = json.loads(raw_map.decode("utf-8")) if raw_map else None


def _redaction(call, err: Error, what: str) -> Redaction:
    handle = ctypes.c_void_p()
    _check(call(ctypes.byref(handle)), err, what)
    try:
        return Redaction(handle)
    finally:
        lib.pb_redaction_free(handle)


def _bias_options(options, bias: Optional[float], type_bias: Optional[Sequence[float]], min_confidence: float,
                  keep: list) -> None:
    """The operating point of pb_detect_options / pb_redact_options; `keep` holds the ctypes arrays alive."""
    if bias is not None:
        options.use_bias, options.bias = 1, bias
    if type_bias:
        array = (ctypes.c_float * len(type_bias))(*type_bias)
        keep.append(array)
        options.type_bias, options.type_bias_count = ctypes.cast(array, ctypes.POINTER(ctypes.c_float)), len(type_bias)
    options.min_confidence = min_confidence


class Detector:
    """One model, or an ensemble (models[0] is the main one), with a post-processing profile: `None` = the one the
    main model declares, else "none". Known profiles: none, secrets-code, secrets-binary, redact."""

    def __init__(self, models: Union[Model, Sequence[Model]], profile: Optional[str] = None):
        self.models = [models] if isinstance(models, Model) else list(models)
        handles = (ctypes.c_void_p * len(self.models))(*[m._handle for m in self.models])
        handle, err = ctypes.c_void_p(), Error()
        _check(lib.pb_detector_create(handles, len(self.models), profile.encode() if profile else None,
                                      ctypes.byref(handle), ctypes.byref(err)), err, "cannot make the detector")
        self._handle = handle

    @property
    def profile(self) -> str:
        return lib.pb_detector_profile(self._handle).decode()

    @property
    def max_input_bytes(self) -> int:
        return lib.pb_detector_max_input_bytes(self._handle)

    def wants_file(self, file_name: str) -> bool:
        return bool(lib.pb_detector_wants_file(self._handle, _encode(file_name)))

    def wants_directory(self, directory_name: str) -> bool:
        return bool(lib.pb_detector_wants_directory(self._handle, _encode(directory_name)))

    def path_severity(self, path: str) -> str:
        """The severity of the findings of a file at `path` inside the project scanned: "error", or "warning" where
        the profile lowers it (secrets-code: test, example and documentation paths)."""
        warning = lib.pb_detector_path_severity(self._handle, _encode(path)) == SEVERITY_WARNING
        return "warning" if warning else "error"

    def detect(self, inputs: Sequence[Tuple[str, bytes]], session: Session, *, reveal: bool = False,
               per_model: bool = False, ensemble: bool = True, votes: int = 0, bias: Optional[float] = None,
               type_bias: Optional[Sequence[float]] = None, min_confidence: float = 0.0, archives: bool = True,
               prefilter: bool = False, strings_only: bool = True, early_exit: bool = False,
               keep_examples: bool = False, queries: Sequence[str] = (),
               paths: Optional[Sequence[Optional[str]]] = None) -> ScanResult:
        """Runs the detector over (name, bytes) inputs. Values are masked unless `reveal` is set. `paths` (one per
        input; None = its name) says where each input is inside the project scanned, for the severity of its findings.
        With the secrets-binary profile, a finding must touch a printable string unless `strings_only` is False (the
        CLI's --all-bytes); other profiles ignore it.
        """
        options, keep = detect_options(), []
        options.flags = ((DETECT_REVEAL if reveal else 0) | (DETECT_PER_MODEL if per_model else 0) |
                         (0 if ensemble else DETECT_NO_ENSEMBLE) | (DETECT_EXPAND_ARCHIVES if archives else 0) |
                         (DETECT_PREFILTER if prefilter else 0) | (DETECT_STRINGS_ONLY if strings_only else 0) |
                         (DETECT_EARLY_EXIT if early_exit else 0) | (DETECT_KEEP_EXAMPLES if keep_examples else 0))
        options.votes = votes
        _bias_options(options, bias, type_bias, min_confidence, keep)
        if queries:
            array = (ctypes.c_char_p * len(queries))(*[q.encode("utf-8") for q in queries])
            keep.append(array)
            options.queries, options.query_count = array, len(queries)
        buffers = [bytes(data) for _, data in inputs]
        items = (Input * len(buffers))(*[Input(ctypes.cast(ctypes.c_char_p(b), ctypes.c_void_p), len(b))
                                         for b in buffers])
        names = (ctypes.c_char_p * len(buffers))(*[_encode(name) for name, _ in inputs])
        if paths is not None:
            if len(paths) != len(buffers):
                raise ArgumentError(f"{len(paths)} paths for {len(buffers)} inputs")
            array = (ctypes.c_char_p * len(buffers))(*[_encode(p) if p else None for p in paths])
            keep.append(array)
            options.paths = array
        out, err = ctypes.c_void_p(), Error()
        _check(lib.pb_detect(session._handle, self._handle, items, len(buffers), names, ctypes.byref(options),
                             ctypes.byref(out), ctypes.byref(err)), err, "the scan failed")
        try:
            return ScanResult(json.loads(ctypes.string_at(out.value).decode("utf-8")))
        finally:
            lib.pb_free(out)

    def redact(self, data: bytes, session: Session, *, types: Optional[Iterable[str]] = None, with_map: bool = False,
               bias: Optional[float] = None, type_bias: Optional[Sequence[float]] = None,
               min_confidence: float = 0.0, name: Optional[str] = None) -> Redaction:
        """A copy of `data` with every detected span replaced by a typed marker such as [EMAIL_1]. Inputs of any
        length are analyzed; an input over the model's limit (max_input_bytes) is refused, as the CLI and the HTTP API
        refuse it. `name` is the input's name in the report and the map (their `input`)."""
        data = bytes(data)
        if len(data) > self.max_input_bytes:
            raise ArgumentError(f"{len(data)} bytes, over this model's limit of {self.max_input_bytes} bytes: "
                                "not redacted")
        options, keep = redact_options(), []
        options.flags = REDACT_WITH_MAP if with_map else 0
        _bias_options(options, bias, type_bias, min_confidence, keep)
        if types:
            options.types = ",".join(types).encode("utf-8")
        if name:
            options.name = name.encode("utf-8")
        err = Error()

        def call(out):
            return lib.pb_redact(session._handle, self._handle, data, len(data), ctypes.byref(options), out,
                                 ctypes.byref(err))

        return _redaction(call, err, "cannot redact")

    def close(self) -> None:
        if getattr(self, "_handle", None):
            lib.pb_detector_free(self._handle)
            self._handle = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def __del__(self):
        try:
            self.close()
        except Exception:  # at interpreter exit the library may already be gone
            pass


# ------------------------------------------------------------------------------------------- redaction by spans


def redact_spans(data: bytes, spans: Iterable[Sequence], type_order: Sequence[str] = (),
                 with_map: bool = False) -> Redaction:
    """Redaction of spans that come from anywhere: [(start, end, type, confidence=1.0, source="model"), ...].
    `type_order` breaks ties between overlapping spans of different types with the same confidence and length."""
    data = bytes(data)
    rows, keep = [], []
    for s in spans:
        start, end, kind = s[0], s[1], s[2]
        confidence = s[3] if len(s) > 3 else 1.0
        source = s[4] if len(s) > 4 else "model"
        kind_b, source_b = kind.encode("utf-8"), source.encode("utf-8")
        keep += [kind_b, source_b]
        rows.append(RedactionSpan(start, end, kind_b, confidence, source_b))
    array = (RedactionSpan * len(rows))(*rows)
    order = (ctypes.c_char_p * len(type_order))(*[t.encode("utf-8") for t in type_order])
    err = Error()
    flags = REDACT_WITH_MAP if with_map else 0
    return _redaction(lambda out: lib.pb_redact_spans(data, len(data), array, len(rows), order, len(type_order), flags,
                                                      out, ctypes.byref(err)), err, "cannot redact")


def restore(redacted: bytes, redaction_map: Union[dict, str]) -> bytes:
    """The original bytes from a redacted copy and its map (by offsets: the copy must not have been edited)."""
    redacted = bytes(redacted)
    text = redaction_map if isinstance(redaction_map, str) else json.dumps(redaction_map)
    out, size, err = ctypes.c_void_p(), ctypes.c_size_t(), Error()
    _check(lib.pb_restore(redacted, len(redacted), text.encode("utf-8"), ctypes.byref(out), ctypes.byref(size),
                          ctypes.byref(err)), err, "cannot restore")
    try:
        return ctypes.string_at(out.value, size.value)
    finally:
        lib.pb_free(out)


# ---------------------------------------------------------------------------------------------------- archives


def expand_archive(data: bytes, name: str = "",
                   max_depth: Optional[int] = None) -> Tuple[List[Tuple[str, bytes]], List[str]]:
    """The members of a zip, gzip or tar archive, recursively and within limits against archive bombs, as
    ([(name, bytes)], ["name: reason" of what could not be extracted]). `max_depth` is 0 to 16 (default 3; a
    larger value counts as 16, a negative one is refused)."""
    data = bytes(data)
    limits = archive_limits()
    if max_depth is not None:
        limits.max_depth = max_depth
    handle, err = ctypes.c_void_p(), Error()
    _check(lib.pb_archive_expand(data, len(data), _encode(name), ctypes.byref(limits), ctypes.byref(handle),
                                 ctypes.byref(err)), err, "cannot expand the archive")
    try:
        members = []
        for i in range(lib.pb_archive_member_count(handle)):
            size = ctypes.c_size_t()
            pointer = lib.pb_archive_member_data(handle, i, ctypes.byref(size))
            members.append((lib.pb_archive_member_name(handle, i).decode("utf-8", "replace"),
                            ctypes.string_at(pointer, size.value) if size.value else b""))
        failures = [lib.pb_archive_failure(handle, i).decode("utf-8", "replace")
                    for i in range(lib.pb_archive_failure_count(handle))]
        return members, failures
    finally:
        lib.pb_archive_free(handle)


# ------------------------------------------------------------------------------------------- one-call helpers

Source = Union[PathLike, bytes, bytearray]


def load_detector(model: PathLike = "secrets-code", profile: Optional[str] = None, ensemble: bool = True,
                  extra_members: Sequence[PathLike] = ()) -> Detector:
    """A detector for an installed specialist (`name` or `name@version`) or a model file, as the CLI's --model: a
    string that holds a folder separator, starts with `.` or ends in `.gguf`, or an os.PathLike, is a model file;
    any other string is a name of the catalog, even when a file of that name is in the current folder. The files of a
    specialist are read once and loaded from the bytes checked against the catalog's SHA-256."""
    if not isinstance(model, str) or _models.looks_like_path(model):
        path, hint = os.fspath(model), ""
        models = [Model(path)]
    else:
        specialist = _models.resolve(model, version(), ensemble)
        hint = specialist.profile
        models = [Model._from_memory(data, path) for path, data in specialist.files]
    models += [Model(os.fspath(m)) for m in (extra_members if ensemble else ())]
    if not profile and not models[0].describe().get("profile"):
        profile = hint or None
    return Detector(models, profile)


def scan(sources: Union[Source, Sequence[Source]], model: PathLike = "secrets-code", *, profile: Optional[str] = None,
         threads: Optional[int] = None, detector: Optional[Detector] = None, session: Optional[Session] = None,
         exclude: Sequence[str] = (), **options) -> ScanResult:
    """Scans files, directories or bytes with an installed specialist or a model file. Directories are walked as the
    CLI walks them: the profile chooses the files and folders, links (junctions included) are not followed, and the
    paths that match `exclude` (the CLI's --exclude) or the folder's .purebyteignore are left out. Files are read and
    analyzed in batches of about 64 MiB; when the library fails on a batch, its inputs are analyzed one by one, and
    one that fails on its own is listed in `failures` with the library's message. `options` are those of
    Detector.detect (reveal, ensemble, votes, bias, min_confidence, archives...). Findings in test, example and
    documentation paths are warnings (`Finding.severity`, `ScanResult.by_severity`)."""
    if isinstance(sources, (str, bytes, bytearray)) or hasattr(sources, "__fspath__"):
        sources = [sources]
    if isinstance(exclude, str):
        exclude = [exclude]
    detector = detector or load_detector(model, profile, options.get("ensemble", True))
    items, failures = _walk.plan(detector, list(sources), exclude)
    if not items:
        return ScanResult({"results": [], "findings": [], "failures": []}, failures)
    session = session or Session(threads)
    analyzed: List[Tuple[dict, int]] = []  # (document, number of inputs it analyzed)

    def analyze(inputs: List[Tuple[str, bytes]], paths: List[Optional[str]]) -> None:
        try:
            analyzed.append((detector.detect(inputs, session, paths=paths, **options).document, len(inputs)))
        except PureByteError as e:
            if e.status == "argument":  # the call itself is wrong (an option), not one of its inputs
                raise
            if len(inputs) == 1:
                failures.append({"file": inputs[0][0], "reason": str(e)})
                return
            for one, path in zip(inputs, paths):
                analyze([one], [path])

    batch: List[Tuple[str, bytes]] = []
    paths: List[Optional[str]] = []
    size = 0
    for item in items:
        if isinstance(item.source, bytes):
            data = item.source
        else:
            try:
                with open(item.source, "rb") as fh:
                    data = fh.read()
            except OSError as e:
                failures.append({"file": item.name, "reason": f"could not be read: {e.strerror}"})
                continue
        batch.append((item.name, data))
        paths.append(item.rule)
        size += len(data)
        if size >= BATCH_BYTES:
            analyze(batch, paths)
            batch, paths, size = [], [], 0
    if batch:
        analyze(batch, paths)
    # One document for the whole scan, whose results give `input` as the index among all the inputs analyzed.
    merged = {key: value for key, value in (analyzed[0][0] if analyzed else {}).items()
              if key not in ("results", "findings", "failures")}
    merged.update({"results": [], "findings": [], "failures": []})
    offset = 0
    for document, count in analyzed:
        for result in document.get("results", []):
            result["input"] = result.get("input", 0) + offset
            merged["results"].append(result)
        merged["findings"] += document.get("findings", [])
        merged["failures"] += document.get("failures", [])
        offset += count
    return ScanResult(merged, failures)


def decide(data: Union[bytes, str], model: PathLike = "secrets-code", *, profile: Optional[str] = None,
           detector: Optional[Detector] = None, session: Optional[Session] = None, **options) -> Dict:
    """One small input (up to 4096 bytes): {"decision": "positive" | "negative", "result": {...}, "findings": [...]}."""
    raw = data.encode("utf-8") if isinstance(data, str) else bytes(data)
    if len(raw) > DECIDE_LIMIT:
        raise ArgumentError(f"decide takes at most {DECIDE_LIMIT} bytes (got {len(raw)}): use scan")
    detector = detector or load_detector(model, profile, options.get("ensemble", True))
    session = session or Session(1)
    result = detector.detect([("", raw)], session, archives=False, **options)
    positive = bool(result.findings)
    if not positive and not any(h.get("type") == "tag" for h in detector.models[0].describe().get("heads", [])):
        decisions = result.results[0].get("decisions", {}) if result.results else {}
        positive = bool(decisions) and next(iter(decisions.values())).get("index", 0) != 0
    return {"decision": "positive" if positive else "negative", "result": result.results[0] if result.results else {},
            "findings": [f.as_dict() for f in result.findings]}


def redact(data: Union[bytes, str], model: PathLike, *, profile: Optional[str] = None,
           types: Optional[Iterable[str]] = None, with_map: bool = False, detector: Optional[Detector] = None,
           session: Optional[Session] = None, **options) -> Redaction:
    """A redacted copy of `data` with a model that finds typed spans (see Detector.redact)."""
    raw = data.encode("utf-8") if isinstance(data, str) else bytes(data)
    detector = detector or load_detector(model, profile)
    session = session or Session()
    return detector.redact(raw, session, types=types, with_map=with_map, **options)
