"""PureByte: tiny byte-level models on the CPU. Bindings (ctypes, standard library only) over libpurebyte.

    import purebyte

    result = purebyte.scan("settings.py", model="secrets-code")
    for finding in result.findings:
        print(finding.file, finding.line, finding.col, finding.snippet_masked)

`model` is an installed specialist (`purebyte models pull NAME`) or the path of a model file. Detected values are
masked unless `reveal=True`. For repeated calls, build a Detector and a Session once and pass them in.
"""
from ._api import (Detector, Finding, Model, PureByteError, Redaction, ScanResult, Session, decide, expand_archive,
                   load_detector, redact, redact_spans, restore, scan, version)
from ._models import models_directory

__version__ = "1.0.0"

__all__ = ["Detector", "Finding", "Model", "PureByteError", "Redaction", "ScanResult", "Session", "decide",
           "expand_archive", "load_detector", "models_directory", "redact", "redact_spans", "restore", "scan",
           "version", "__version__"]
