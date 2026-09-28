"""ctypes declarations of the C API (engine/include/purebyte/pb.h) and the loading of the shared library.

The library is found, in order: at the path in PUREBYTE_LIBRARY, then next to this module (purebyte/_lib/, where a
wheel puts it). Every declaration below mirrors pb.h; keep them in the same order when the header changes.
"""
import ctypes
import os
import sys
from ctypes import (POINTER, Structure, c_char, c_char_p, c_float, c_int32, c_int64, c_size_t, c_uint8, c_uint32,
                    c_uint64, c_void_p)

ABI_VERSION = 1

# ------------------------------------------------------------------------------------------------------- structures


class Error(Structure):
    _fields_ = [("status", c_int32), ("message", c_char * 508)]


class SessionOptions(Structure):
    _fields_ = [("struct_size", c_uint32), ("threads", c_int32), ("intra_threads", c_int32), ("kernel", c_char_p)]


class Input(Structure):
    _fields_ = [("data", c_void_p), ("size", c_size_t)]


class DetectOptions(Structure):
    _fields_ = [("struct_size", c_uint32), ("flags", c_uint32), ("votes", c_int32), ("use_bias", c_int32),
                ("bias", c_float), ("type_bias", POINTER(c_float)), ("type_bias_count", c_uint32),
                ("min_confidence", c_float), ("queries", POINTER(c_char_p)), ("query_count", c_uint32),
                ("paths", POINTER(c_char_p))]


class RedactOptions(Structure):
    _fields_ = [("struct_size", c_uint32), ("flags", c_uint32), ("use_bias", c_int32), ("bias", c_float),
                ("type_bias", POINTER(c_float)), ("type_bias_count", c_uint32), ("min_confidence", c_float),
                ("types", c_char_p), ("name", c_char_p)]


class RedactionSpan(Structure):
    _fields_ = [("start", c_int64), ("end", c_int64), ("type", c_char_p), ("confidence", c_float),
                ("source", c_char_p)]


class ArchiveLimits(Structure):
    _fields_ = [("struct_size", c_uint32), ("max_depth", c_int32), ("max_member_bytes", c_uint64),
                ("max_total_bytes", c_uint64), ("max_ratio", c_uint64)]


# The *_init functions of pb.h are static inline (compiled into each caller, so that a struct always carries the size
# its caller knows): these mirror them. ctypes structures start zeroed.
def session_options() -> SessionOptions:
    o = SessionOptions()
    o.struct_size, o.threads, o.intra_threads, o.kernel = ctypes.sizeof(o), 1, 0, b"auto"
    return o


def detect_options() -> DetectOptions:
    o = DetectOptions()
    o.struct_size = ctypes.sizeof(o)
    return o


def redact_options() -> RedactOptions:
    o = RedactOptions()
    o.struct_size = ctypes.sizeof(o)
    return o


def archive_limits() -> ArchiveLimits:
    o = ArchiveLimits()
    o.struct_size, o.max_depth, o.max_member_bytes, o.max_total_bytes, o.max_ratio = (
        ctypes.sizeof(o), 3, 64 << 20, 256 << 20, 200)  # PB_ARCHIVE_MAX_*
    return o


# pb_detect_options.flags
DETECT_REVEAL = 0x1
DETECT_PER_MODEL = 0x2
DETECT_KEEP_EXAMPLES = 0x4
DETECT_PREFILTER = 0x8
DETECT_STRINGS_ONLY = 0x10
DETECT_EARLY_EXIT = 0x20
DETECT_EXPAND_ARCHIVES = 0x40
DETECT_NO_ENSEMBLE = 0x80
# pb_redact_options.flags
REDACT_WITH_MAP = 0x1
# pb_detector_path_severity
SEVERITY_ERROR = 0
SEVERITY_WARNING = 1

_P = c_void_p  # opaque handles: pb_model*, pb_session*, pb_detector*, pb_redaction*, pb_archive*
_STATUS = c_int32

# name: (result type, argument types)
_FUNCTIONS = {
    "pb_abi_version": (c_uint32, []),
    "pb_version": (c_char_p, []),
    "pb_format_version": (c_uint32, []),
    "pb_status_name": (c_char_p, [c_int32]),
    "pb_free": (None, [c_void_p]),
    "pb_model_load": (_STATUS, [c_char_p, POINTER(_P), POINTER(Error)]),
    "pb_model_load_memory": (_STATUS, [c_void_p, c_size_t, POINTER(_P), POINTER(Error)]),
    "pb_model_free": (None, [_P]),
    "pb_model_describe": (c_char_p, [_P]),
    "pb_session_create": (_STATUS, [POINTER(SessionOptions), POINTER(_P), POINTER(Error)]),
    "pb_session_free": (None, [_P]),
    "pb_session_kernel": (c_char_p, [_P]),
    "pb_session_threads": (c_int32, [_P]),
    "pb_detector_create": (_STATUS, [POINTER(_P), c_size_t, c_char_p, POINTER(_P), POINTER(Error)]),
    "pb_detector_free": (None, [_P]),
    "pb_detector_profile": (c_char_p, [_P]),
    "pb_detector_wants_file": (c_int32, [_P, c_char_p]),
    "pb_detector_wants_directory": (c_int32, [_P, c_char_p]),
    "pb_detector_max_input_bytes": (c_uint64, [_P]),
    "pb_detector_path_severity": (c_int32, [_P, c_char_p]),
    "pb_detect": (_STATUS, [_P, _P, POINTER(Input), c_size_t, POINTER(c_char_p), POINTER(DetectOptions),
                            POINTER(c_void_p), POINTER(Error)]),
    "pb_redact": (_STATUS, [_P, _P, c_void_p, c_size_t, POINTER(RedactOptions), POINTER(_P), POINTER(Error)]),
    "pb_redaction_output": (POINTER(c_uint8), [_P, POINTER(c_size_t)]),
    "pb_redaction_report": (c_char_p, [_P]),
    "pb_redaction_map": (c_char_p, [_P]),
    "pb_redaction_free": (None, [_P]),
    "pb_redact_spans": (_STATUS, [c_void_p, c_size_t, POINTER(RedactionSpan), c_size_t, POINTER(c_char_p), c_size_t,
                                  c_uint32, POINTER(_P), POINTER(Error)]),
    "pb_restore": (_STATUS, [c_void_p, c_size_t, c_char_p, POINTER(c_void_p), POINTER(c_size_t), POINTER(Error)]),
    "pb_is_archive": (c_int32, [c_void_p, c_size_t]),
    "pb_archive_expand": (_STATUS, [c_void_p, c_size_t, c_char_p, POINTER(ArchiveLimits), POINTER(_P),
                                    POINTER(Error)]),
    "pb_archive_member_count": (c_size_t, [_P]),
    "pb_archive_member_name": (c_char_p, [_P, c_size_t]),
    "pb_archive_member_data": (POINTER(c_uint8), [_P, c_size_t, POINTER(c_size_t)]),
    "pb_archive_failure_count": (c_size_t, [_P]),
    "pb_archive_failure": (c_char_p, [_P, c_size_t]),
    "pb_archive_free": (None, [_P]),
}


def library_file_name() -> str:
    if sys.platform == "win32":
        return "purebyte.dll"
    if sys.platform == "darwin":
        return "libpurebyte.dylib"
    return "libpurebyte.so"


def _library_path() -> str:
    explicit = os.environ.get("PUREBYTE_LIBRARY")
    if explicit:
        return explicit
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "_lib", library_file_name())


def _load():
    path = _library_path()
    if not os.path.isfile(path):
        raise ImportError(f"the PureByte library was not found at {path}: reinstall the package, or set "
                          f"PUREBYTE_LIBRARY to the path of {library_file_name()}")
    lib = ctypes.CDLL(path)
    for name, (result, arguments) in _FUNCTIONS.items():
        function = getattr(lib, name)
        function.restype = result
        function.argtypes = arguments
    if lib.pb_abi_version() != ABI_VERSION:
        raise ImportError(f"{path} has ABI version {lib.pb_abi_version()}; this package needs {ABI_VERSION}: "
                          "install matching versions of the package and the library")
    return lib


lib = _load()
