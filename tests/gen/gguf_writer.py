"""A minimal GGUF version 3 writer (spec/FORMAT.md, section 2), standard library only.

    w = GGUFWriter()
    w.add("general.architecture", "purebyte")
    w.add("purebyte.d_model", 16)                          # int -> i32, float -> f32, bool -> bool, str -> string
    w.add("purebyte.ngram.orders", [2, 3])                 # lists of one of those types -> arrays
    w.add_f32("embed.weight", [256, 16], values)          # shape in row-major order, values flat (floats)
    w.add_i8("blocks.0.in_proj.weight.packed", [96, 4], data)   # raw bytes
    open(path, "wb").write(w.tobytes())
"""
import struct

ALIGNMENT = 32

# GGUF metadata value types
U32, I32, F32, BOOL, STRING, ARRAY, I64 = 4, 5, 6, 7, 8, 9, 11
# GGML tensor types used by PureByte models
TENSOR_F32, TENSOR_I8 = 0, 24


def _string(s):
    data = s.encode("utf-8")
    return struct.pack("<Q", len(data)) + data


def _scalar_type(value):
    if isinstance(value, bool):
        return BOOL
    if isinstance(value, int):
        return I32 if -2**31 <= value < 2**31 else I64
    if isinstance(value, float):
        return F32
    if isinstance(value, str):
        return STRING
    raise TypeError(f"unsupported metadata value {value!r}")


def _scalar(kind, value):
    if kind == BOOL:
        return struct.pack("<B", 1 if value else 0)
    if kind == I32:
        return struct.pack("<i", value)
    if kind == I64:
        return struct.pack("<q", value)
    if kind == U32:
        return struct.pack("<I", value)
    if kind == F32:
        return struct.pack("<f", value)
    if kind == STRING:
        return _string(value)
    raise TypeError(kind)


class GGUFWriter:
    def __init__(self):
        self.metadata = []  # (key, type, encoded value)
        self.tensors = []   # (name, shape, type, data)

    def add(self, key, value, kind=None):
        """Adds a metadata key. Lists become arrays of the type of their first element (an empty list: i32)."""
        if isinstance(value, (list, tuple)):
            element = kind or (_scalar_type(value[0]) if value else I32)
            if element == I32 and any(not -2**31 <= v < 2**31 for v in value):
                element = I64
            body = struct.pack("<IQ", element, len(value)) + b"".join(_scalar(element, v) for v in value)
            self.metadata.append((key, ARRAY, body))
        else:
            kind = kind or _scalar_type(value)
            self.metadata.append((key, kind, _scalar(kind, value)))

    def add_f32(self, name, shape, values):
        values = list(values)
        count = 1
        for n in shape:
            count *= n
        if len(values) != count:
            raise ValueError(f"{name}: {len(values)} values for shape {shape}")
        self.tensors.append((name, list(shape), TENSOR_F32, struct.pack(f"<{count}f", *values)))

    def add_i8(self, name, shape, data):
        count = 1
        for n in shape:
            count *= n
        if len(data) != count:
            raise ValueError(f"{name}: {len(data)} bytes for shape {shape}")
        self.tensors.append((name, list(shape), TENSOR_I8, bytes(data)))

    def tobytes(self):
        out = bytearray(struct.pack("<IIQQ", 0x46554747, 3, len(self.tensors), len(self.metadata)))
        for key, kind, body in self.metadata:
            out += _string(key) + struct.pack("<I", kind) + body
        offset = 0
        for name, shape, kind, data in self.tensors:
            dims = list(reversed(shape))  # GGUF: fastest-varying dimension first
            out += _string(name) + struct.pack("<I", len(dims)) + struct.pack(f"<{len(dims)}Q", *dims)
            out += struct.pack("<IQ", kind, offset)
            offset += (len(data) + ALIGNMENT - 1) // ALIGNMENT * ALIGNMENT
        out += bytes(-len(out) % ALIGNMENT)
        for _name, _shape, _kind, data in self.tensors:
            out += data + bytes(-len(data) % ALIGNMENT)
        return bytes(out)
