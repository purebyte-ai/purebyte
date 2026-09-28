"""Files the runtime must refuse (spec/FORMAT.md, section 13): every case is a valid random model with one thing
broken, and the engine must refuse it with the right status and a message that names the problem. The reference
reader (reference/purebyte_ref/gguf.py) must refuse the broken containers (section 2) too, and both must read a
container whose strings are not UTF-8.

    python tests/python/test_validation.py --model-dump build/tests/model_dump --work build/tests/work
"""
import os
import struct
import subprocess
import sys

import common
from random_models import (Builder, attention_lookahead, ngram_gated_4bit, query_extraction, ssm_bidirectional,
                           ssm_causal_ternary)


def set_meta(w, key, value, kind=None):
    w.metadata = [m for m in w.metadata if m[0] != key]
    w.add(key, value, kind)


def drop_meta(w, key):
    w.metadata = [m for m in w.metadata if m[0] != key]


def drop_tensor(w, name):
    w.tensors = [t for t in w.tensors if t[0] != name]


def replace_tensor(w, name, shape=None, data=None):
    w.tensors = [(n, shape if (n == name and shape) else s, k, data if (n == name and data is not None) else d)
                 for n, s, k, d in w.tensors]


def tensor_data(w, name):
    return next(d for n, _, _, d in w.tensors if n == name)


def raw_meta(w, key, kind, body):
    """A metadata value written byte for byte, as a writer that breaks the rules would."""
    w.metadata = [m for m in w.metadata if m[0] != key] + [(key, kind, body)]


def set_offset(blob, name, offset):
    """The file `blob` with the data offset of tensor `name` rewritten."""
    blob = bytearray(blob)
    info = struct.pack("<Q", len(name)) + name.encode()
    at = blob.index(info) + len(info)
    (rank,) = struct.unpack_from("<I", blob, at)
    at += 4 + 8 * rank + 4
    blob[at:at + 8] = struct.pack("<Q", offset)
    return bytes(blob)


def base():
    return ssm_causal_ternary().w


def case_bad_magic():
    return b"GGUX" + base().tobytes()[4:]


def case_truncated():
    return base().tobytes()[:-100]


def case_gguf_version():
    blob = bytearray(base().tobytes())
    blob[4:8] = struct.pack("<I", 2)
    return bytes(blob)


def case_alignment():
    w = base()
    set_meta(w, "general.alignment", 4)
    return w.tobytes()


def case_duplicate_key():
    w = base()
    w.add("purebyte.d_model", 32)
    return w.tobytes()


def case_architecture():
    w = base()
    set_meta(w, "general.architecture", "llama")
    return w.tobytes()


def case_newer_version():
    w = base()
    set_meta(w, "purebyte.format_version", 4)
    return w.tobytes()


def case_older_version():
    w = base()
    set_meta(w, "purebyte.format_version", 1)
    return w.tobytes()


def case_required_feature():
    w = base()
    set_meta(w, "purebyte.required_features", ["attention", "quantum"])
    return w.tobytes()


def case_block_type():
    w = base()
    set_meta(w, "purebyte.block.1.type", "transformer")
    return w.tobytes()


def case_head_type():
    w = base()
    set_meta(w, "purebyte.head.1.type", "regression")
    return w.tobytes()


def case_missing_tensor():
    w = base()
    drop_tensor(w, "out_norm.weight")
    return w.tobytes()


def case_unused_tensor():
    w = base()
    w.add_f32("blocks.0.extra.weight", [4], [0.0] * 4)
    return w.tobytes()


def case_wrong_shape():
    w = base()
    replace_tensor(w, "embed.weight", shape=[32, 256])
    return w.tobytes()


def case_ternary_code():
    w = base()
    data = bytearray(tensor_data(w, "blocks.0.in_proj.weight.packed"))
    data[5] |= 0b11  # code 11 is not ternary
    replace_tensor(w, "blocks.0.in_proj.weight.packed", data=bytes(data))
    return w.tobytes()


def case_ternary_padding():
    b = Builder("padding", 10, 1)
    b.ternary(2)
    b.ssm_defaults(H=1, P=8, N=8, G=1, K=4)
    b.ssm(0, 1, 8, 8, 1, 4, True)
    b.final()
    b.mlp(0, "last_mean", 8, 2, 4.0)
    w = b.w
    data = bytearray(tensor_data(w, "blocks.0.in_proj.weight.packed"))  # 10 codes in 3 bytes: codes 10, 11 are padding
    data[2] |= 0b01 << 4
    replace_tensor(w, "blocks.0.in_proj.weight.packed", data=bytes(data))
    return w.tobytes()


def case_stride():
    w = base()
    set_meta(w, "purebyte.window.stride", 65)
    return w.tobytes()


def case_gate_target():
    w = base()
    set_meta(w, "purebyte.head.1.gated_by", 0)  # a tag head cannot gate
    return w.tobytes()


def case_ngram_orders():
    w = base()
    set_meta(w, "purebyte.ngram.orders", [1, 2, 3])
    return w.tobytes()


def case_empty_orders():
    w = base()
    set_meta(w, "purebyte.ngram.orders", [])  # declares no n-grams, and the file has tables
    return w.tobytes()


def case_label_count():
    w = base()
    set_meta(w, "purebyte.head.1.labels", ["a", "b", "c"])
    return w.tobytes()


def case_exit_layer():
    w = base()
    set_meta(w, "purebyte.head.2.layer", 3)
    return w.tobytes()


def case_stream_attention():
    w = attention_lookahead().w
    set_meta(w, "purebyte.context", "stream")
    return w.tobytes()


def case_direction():
    w = base()
    set_meta(w, "purebyte.direction", "sideways")
    return w.tobytes()


def case_pooling():
    w = base()
    set_meta(w, "purebyte.head.1.pooling", "median")
    return w.tobytes()


def case_scheme():
    w = base()
    set_meta(w, "purebyte.head.0.scheme", "BIO:O,B-e,I-e")
    return w.tobytes()


def case_hash():
    w = base()
    set_meta(w, "purebyte.ngram.hash", "fnv1a")
    return w.tobytes()


def case_ngram_bits():
    w = base()
    set_meta(w, "purebyte.ngram.bits", 8)
    return w.tobytes()


def case_key_type():
    w = base()
    set_meta(w, "purebyte.d_model", "32")
    return w.tobytes()


def case_tampered_dims():
    w = base()
    w.tensors = [(n, [s[0] * 1000] + s[1:] if n == "embed.weight" else s, k, d) for n, s, k, d in w.tensors]
    return w.tobytes()


def case_groups():
    w = ssm_bidirectional().w
    set_meta(w, "purebyte.n_groups", 3)
    return w.tobytes()


def case_query_region():
    w = base()
    set_meta(w, "purebyte.query.region", 64)
    set_meta(w, "purebyte.query.max", 2)
    return w.tobytes()


def case_second_exit():
    w = base()
    set_meta(w, "purebyte.head.count", 4)
    set_meta(w, "purebyte.head.3.type", "exit")
    set_meta(w, "purebyte.head.3.features", "rms_max_mean")
    set_meta(w, "purebyte.head.3.layer", 1)
    set_meta(w, "purebyte.head.3.threshold", 0.0)
    w.add_f32("heads.3.proj.weight", [1, 64], [0.0] * 64)
    w.add_f32("heads.3.proj.bias", [1], [0.0])
    return w.tobytes()


def case_tensor_type():
    w = base()
    w.tensors = [(n, s, 1 if n == "out_norm.weight" else k, d) for n, s, k, d in w.tensors]  # GGML F16
    return w.tobytes()


def case_duplicate_tensor():
    w = base()
    w.tensors.append(next(t for t in w.tensors if t[0] == "out_norm.weight"))
    return w.tobytes()


def case_misaligned_tensor():
    w = base()
    return set_offset(w.tobytes(), w.tensors[0][0], 4)  # the writer's alignment is 32


def case_overlapping_tensors():
    w = base()
    return set_offset(w.tobytes(), w.tensors[1][0], 0)  # the second tensor laid over the first


def case_boolean():
    w = base()
    raw_meta(w, "purebyte.ngram.gate", 7, b"\x02")
    return w.tobytes()


def case_nesting():
    body = struct.pack("<IQ", 5, 1) + struct.pack("<i", 0)  # arrays of one item, 5 levels deep
    for _ in range(4):
        body = struct.pack("<IQ", 9, 1) + body
    w = base()
    raw_meta(w, "purebyte.nested", 9, body)
    return w.tobytes()


def case_integer_range():
    w = base()
    raw_meta(w, "purebyte.d_model", 10, struct.pack("<Q", 1 << 63))  # a u64 that no int64 holds
    return w.tobytes()


def case_truncated_string():
    w = base()
    raw_meta(w, "general.name", 8, struct.pack("<Q", 1 << 40) + b"x")
    return w.tobytes()


def case_value_count():
    n = (1 << 20) + 1
    w = base()
    raw_meta(w, "purebyte.values", 9, struct.pack("<IQ", 0, n) + bytes(n))  # more values than a file may hold
    return w.tobytes()


def case_non_finite_tensor():
    w = base()
    data = bytearray(tensor_data(w, "out_norm.weight"))
    data[4:8] = struct.pack("<f", float("nan"))
    replace_tensor(w, "out_norm.weight", data=bytes(data))
    return w.tobytes()


def case_non_finite_metadata():
    w = base()
    set_meta(w, "purebyte.head.2.threshold", float("inf"))
    return w.tobytes()


def case_narrowed_integer():
    w = base()
    set_meta(w, "purebyte.d_model", 2**32 + 32)  # 32 once narrowed to 32 bits
    return w.tobytes()


def case_exit_features():
    w = base()
    drop_meta(w, "purebyte.head.2.features")
    return w.tobytes()


def case_scalar_shape():
    w = ngram_gated_4bit().w
    replace_tensor(w, "ngram.gate_bias", shape=[1, 1])  # a scalar is [] or [1]
    return w.tobytes()


def case_gate_range():
    w = base()
    set_meta(w, "purebyte.head.0.gated_by", 64)
    return w.tobytes()


def case_window_min():
    w = base()
    set_meta(w, "purebyte.window.min", 65)  # the window holds 64 bytes
    return w.tobytes()


def case_window_cost():
    w = base()
    set_meta(w, "purebyte.window.size", 1 << 20)
    return w.tobytes()


def case_attention_window():
    w = attention_lookahead().w
    set_meta(w, "purebyte.window.size", 8193)
    return w.tobytes()


def case_second_tag():
    b = ssm_causal_ternary()
    b.tag(3, 1)
    set_meta(b.w, "purebyte.head.count", 4)
    return b.tobytes()


def model_with_pad_ff():
    """A query model whose pad is byte 0xFF, a string that is not UTF-8: valid (spec/FORMAT.md, section 12.3)."""
    w = query_extraction().w
    raw_meta(w, "purebyte.query.pad", 8, struct.pack("<Q", 1) + b"\xff")
    return w.tobytes()


# name: (builder, expected status, text the message must contain)
CASES = {
    "bad magic": (case_bad_magic, "format", "not a GGUF file"),
    "truncated file": (case_truncated, "format", "truncated"),
    "GGUF version 2": (case_gguf_version, "unsupported", "GGUF version 2"),
    "alignment 4": (case_alignment, "format", "general.alignment"),
    "duplicate key": (case_duplicate_key, "format", "appears twice"),
    "other architecture": (case_architecture, "format", "llama"),
    "newer format": (case_newer_version, "unsupported", "version 4"),
    "older format": (case_older_version, "format", "older than 2"),
    "unknown feature": (case_required_feature, "unsupported", "quantum"),
    "unknown block type": (case_block_type, "unsupported", "transformer"),
    "unknown head type": (case_head_type, "unsupported", "regression"),
    "missing tensor": (case_missing_tensor, "format", "out_norm.weight"),
    "unused tensor": (case_unused_tensor, "unsupported", "blocks.0.extra.weight"),
    "wrong shape": (case_wrong_shape, "format", "embed.weight"),
    "ternary code 11": (case_ternary_code, "format", "invalid ternary code"),
    "ternary padding": (case_ternary_padding, "format", "invalid ternary code"),
    "stride over window": (case_stride, "format", "stride"),
    "tag head as gate": (case_gate_target, "format", "is gated by head"),
    "n-gram orders": (case_ngram_orders, "format", "purebyte.ngram.orders"),
    "no n-gram orders declared": (case_empty_orders, "format", "purebyte.ngram.orders"),
    "label count": (case_label_count, "format", "names 3 classes"),
    "exit layer": (case_exit_layer, "format", "purebyte.head.2.layer"),
    "stream with attention": (case_stream_attention, "format", "stream context"),
    "unknown direction": (case_direction, "unsupported", "sideways"),
    "unknown pooling": (case_pooling, "unsupported", "median"),
    "unknown scheme": (case_scheme, "unsupported", "BIO:O,B-e,I-e"),
    "unknown hash": (case_hash, "unsupported", "fnv1a"),
    "n-gram bits": (case_ngram_bits, "unsupported", "bits"),
    "key type": (case_key_type, "format", "purebyte.d_model"),
    "tampered dimensions": (case_tampered_dims, "format", "embed.weight"),
    "heads and groups": (case_groups, "format", "groups"),
    "query region": (case_query_region, "format", "query region"),
    "two exit heads": (case_second_exit, "format", "more than one exit head"),
    "tensor type": (case_tensor_type, "unsupported", "GGML type 1"),
    "duplicate tensor": (case_duplicate_tensor, "format", "appears twice"),
    "misaligned tensor": (case_misaligned_tensor, "format", "not aligned"),
    "overlapping tensors": (case_overlapping_tensors, "format", "overlap"),
    "boolean 2": (case_boolean, "format", "boolean"),
    "arrays 5 deep": (case_nesting, "format", "nested"),
    "u64 out of range": (case_integer_range, "format", "out of range"),
    "truncated string": (case_truncated_string, "format", "truncated"),
    "too many values": (case_value_count, "format", "too many metadata values"),
    "non-finite tensor": (case_non_finite_tensor, "format", "out_norm.weight"),
    "non-finite metadata": (case_non_finite_metadata, "format", "purebyte.head.2.threshold"),
    "narrowed integer": (case_narrowed_integer, "format", "4294967328"),
    "exit head features": (case_exit_features, "format", "purebyte.head.2.features"),
    "scalar shape": (case_scalar_shape, "format", "ngram.gate_bias"),
    "gate out of range": (case_gate_range, "format", "purebyte.head.0.gated_by"),
    "shortest window": (case_window_min, "format", "purebyte.window.min"),
    "window cost": (case_window_cost, "format", "widest row"),
    "attention window": (case_attention_window, "format", "attention"),
    "two tag heads": (case_second_tag, "format", "second tag head"),
}
# The broken containers (spec/FORMAT.md, section 2), which the reference reader refuses too.
CONTAINER = ["bad magic", "truncated file", "GGUF version 2", "alignment 4", "duplicate key", "tampered dimensions",
             "tensor type", "duplicate tensor", "misaligned tensor", "overlapping tensors", "boolean 2", "arrays 5 deep",
             "u64 out of range", "truncated string", "too many values"]


def reference_refuses(blob, status):
    from purebyte_ref import gguf
    try:
        gguf.GGUF(blob)
    except gguf.UnsupportedError:
        return status == "unsupported"
    except gguf.FormatError:
        return status == "format"
    return False


def reference_reads_pad_ff(blob):
    from purebyte_ref import Model, gguf
    return Model(gguf.GGUF(blob)).query_pad == b"\xff"


def main():
    args = common.arguments(__doc__)
    folder = os.path.join(args.work, "invalid")
    os.makedirs(folder, exist_ok=True)
    inputs = os.path.join(folder, "inputs.bin")
    common.write_if_changed(inputs, common.random_models.encode_inputs([b"x" * 100]))
    failures = 0
    for name, (build, status, text) in CASES.items():
        path = os.path.join(folder, name.replace(" ", "-") + ".gguf")
        common.write_if_changed(path, build())
        done = subprocess.run([args.model_dump, path, inputs], capture_output=True)
        message = done.stderr.decode(errors="replace").strip()
        ok = done.returncode == 2 and f": {status}: " in message and text in message
        failures += not ok
        print(f"{'ok  ' if ok else 'FAIL'} {name}: {message or f'exit status {done.returncode}, no message'}")
    print(f"validation: {len(CASES) - failures}/{len(CASES)} files refused as the specification says")

    pad = os.path.join(folder, "query-pad-ff.gguf")
    common.write_if_changed(pad, model_with_pad_ff())
    options = common.variants_for("query-extraction")["default"]
    done = subprocess.run([args.model_dump, pad, inputs, *options], capture_output=True)
    ok = done.returncode == 0
    failures += not ok
    print(f"{'ok  ' if ok else 'FAIL'} the engine reads a query pad that is not UTF-8 (exit status {done.returncode})")
    try:
        import numpy  # noqa: F401
    except ImportError:
        print("NumPy is not installed: the reference reader is not checked")
        return 1 if failures else 0
    for name in CONTAINER:
        build, status, _ = CASES[name]
        ok = reference_refuses(build(), status)
        failures += not ok
        print(f"{'ok  ' if ok else 'FAIL'} the reference refuses {name} ({status})")
    with open(pad, "rb") as f:
        ok = reference_reads_pad_ff(f.read())
    failures += not ok
    print(f"{'ok  ' if ok else 'FAIL'} the reference reads a query pad that is not UTF-8")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
