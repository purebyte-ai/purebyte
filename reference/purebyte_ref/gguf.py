"""GGUF version 3 reader (spec/FORMAT.md, section 2). Tensors come back as NumPy arrays in row-major shape.

The container is checked as section 2 says, with the engine's limits. The checks of the model itself (section 13)
are the engine's: this implementation assumes a model the engine loads."""
import struct

import numpy as np

_SCALARS = {0: "<B", 1: "<b", 2: "<H", 3: "<h", 4: "<I", 5: "<i", 6: "<f", 7: "<B", 10: "<Q", 11: "<q", 12: "<d"}
BOOL, STRING, ARRAY, U64 = 7, 8, 9, 10
F32, I8 = 0, 24
MAX_ENTRIES = 1 << 20  # metadata keys, and tensors
MAX_VALUES = 1 << 20  # metadata values, array items included
MAX_DEPTH = MAX_RANK = 4


class FormatError(ValueError):
    pass


class UnsupportedError(FormatError):
    """A GGUF version or a tensor type this version does not read (PB_ERR_UNSUPPORTED in the engine)."""


class GGUF:
    def __init__(self, blob):
        self.blob = blob
        self.at = 0
        self.budget = MAX_VALUES
        if len(blob) < 24 or self._read("<I")[0] != 0x46554747:
            raise FormatError("not a GGUF file")
        version, n_tensors, n_metadata = self._read("<IQQ")
        if version != 3:
            raise UnsupportedError(f"GGUF version {version}")
        if n_tensors > MAX_ENTRIES or n_metadata > MAX_ENTRIES:
            raise FormatError("absurd tensor or metadata count")
        self.metadata = {}
        for _ in range(n_metadata):
            key = self._string()
            (kind,) = self._read("<I")
            value = self._value(kind, 0)
            if key in self.metadata:
                raise FormatError(f"metadata key {key} appears twice")
            self.metadata[key] = value
        alignment = self.metadata.get("general.alignment", 32)
        if type(alignment) is not int or not 8 <= alignment <= 65536 or alignment & (alignment - 1):
            raise FormatError("general.alignment must be a power of two within 8..65536")
        infos, names = [], set()
        for _ in range(n_tensors):
            name = self._string()
            (rank,) = self._read("<I")
            if rank > MAX_RANK:
                raise FormatError(f"tensor {name} has {rank} dimensions")
            dims = self._read(f"<{rank}Q") if rank else ()
            count = 1
            for n in dims:
                count *= n
                if n == 0 or count > 1 << 40:
                    raise FormatError(f"tensor {name} declares impossible dimensions")
            kind, offset = self._read("<IQ")
            if kind not in (F32, I8):
                raise UnsupportedError(f"tensor {name} has GGML type {kind}")
            if name in names:
                raise FormatError(f"tensor {name} appears twice")
            names.add(name)
            infos.append((name, tuple(reversed(dims)), kind, offset, count))
        base = (self.at + alignment - 1) // alignment * alignment
        self.tensors = {}
        for name, shape, kind, offset, count in infos:
            dtype = np.float32 if kind == F32 else np.uint8
            if offset % alignment:
                raise FormatError(f"tensor {name} is not aligned to {alignment} bytes")
            if base + offset + count * np.dtype(dtype).itemsize > len(blob):
                raise FormatError(f"tensor {name} lies outside the file")
            self.tensors[name] = np.frombuffer(blob, dtype=dtype, count=count, offset=base + offset).reshape(shape)
        # No two tensors share a byte (sorted by offset, a tensor that overlaps any other overlaps the next one).
        extents = sorted((offset, offset + count * (4 if kind == F32 else 1), name)
                         for name, _, kind, offset, count in infos)
        for (_, end, first), (start, _, second) in zip(extents, extents[1:]):
            if end > start:
                raise FormatError(f"tensors {first} and {second} overlap in the file")

    @classmethod
    def open(cls, path):
        with open(path, "rb") as f:
            return cls(f.read())

    def _read(self, fmt):
        size = struct.calcsize(fmt)
        if size > len(self.blob) - self.at:
            raise FormatError(f"truncated at byte {self.at}")
        values = struct.unpack_from(fmt, self.blob, self.at)
        self.at += size
        return values

    def _string(self):
        (n,) = self._read("<Q")
        if n > len(self.blob) - self.at:
            raise FormatError(f"truncated at byte {self.at}")
        # Any bytes: "surrogateescape" keeps those that are not UTF-8, and encoding with it gives them back.
        s = self.blob[self.at:self.at + n].decode("utf-8", "surrogateescape")
        self.at += n
        return s

    def _value(self, kind, depth):
        if self.budget == 0:
            raise FormatError("too many metadata values")
        self.budget -= 1
        if kind == STRING:
            return self._string()
        if kind == ARRAY:
            if depth >= MAX_DEPTH:
                raise FormatError("arrays nested too deeply")
            (element,) = self._read("<I")
            if element > 12:
                raise FormatError(f"unknown array element type {element}")
            (n,) = self._read("<Q")
            if n > 1 << 28:
                raise FormatError("array too long")
            return [self._value(element, depth + 1) for _ in range(n)]
        if kind not in _SCALARS:
            raise FormatError(f"unknown metadata type {kind}")
        (v,) = self._read(_SCALARS[kind])
        if kind == BOOL and v > 1:
            raise FormatError("boolean that is neither 0 nor 1")
        if kind == U64 and v >= 1 << 63:
            raise FormatError("integer out of range")
        return bool(v) if kind == BOOL else v

    # typed access with the defaults of the specification
    def get(self, key, default=None):
        return self.metadata.get(key, default)

    def require(self, key):
        if key not in self.metadata:
            raise FormatError(f"metadata {key} is missing")
        return self.metadata[key]
