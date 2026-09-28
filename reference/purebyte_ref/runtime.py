"""Running a model over inputs (spec/FORMAT.md, section 12): windows or streams, heads with gating, digests."""
import struct

import numpy as np

from .numerics import F32


def windows(size, window, stride, min_length):
    """(start, length) of the windows of an input of `size` bytes (spec 12.1)."""
    out, start = [], 0
    while True:
        length = min(window, size - start)
        if length < min_length:
            break
        out.append((start, length))
        if start + window >= size:
            return out
        start += stride
    # The next window would be shorter than min_length: one more window, aligned to the end, covers the bytes left.
    if out:
        last = max(0, size - window)
        if last > out[-1][0]:
            out.append((last, size - last))
    return out


def fnv1a(data, h=0xCBF29CE484222325):
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def run_heads(model, H, first, bias=None, ungated=False):
    """Every head's output on hidden states H (spec 11.9): ungated heads first, then the gated ones."""
    outputs = [None] * len(model.heads)

    def run(head):
        if head.type == "tag":
            return head.run(H, first, bias)
        return head.run(H, first)

    for head in model.heads:
        if head.gated_by < 0 and head is not model.exit:
            outputs[head.index] = run(head)
    gated = False
    for head in model.heads:
        if head.gated_by < 0:
            continue
        gate = model.heads[head.gated_by]
        if ungated or (outputs[gate.index] is not None and gate.positive(outputs[gate.index])):
            outputs[head.index] = run(head)
        else:
            gated = True
    return outputs, gated


def window_result(model, H, first, offset, bias, ungated):
    outputs, gated = run_heads(model, H, first, bias, ungated)
    digest = fnv1a(np.ascontiguousarray(H, dtype="<f4").tobytes())
    for out in outputs:
        if out is not None:
            digest = fnv1a(np.asarray(out["values"], dtype="<f4").tobytes(), digest)
            for s in out.get("spans", []):
                s[0] += offset
                s[1] += offset
    label, p_positive = 0, 0.0
    if model.first_choice >= 0 and outputs[model.first_choice] is not None:
        c = outputs[model.first_choice]
        label, p_positive = c["label"], float(F32(1.0) - c["values"][0])
    return {"flags": 4 if gated else 0, "label": label, "p_positive": p_positive, "digest": f"{digest:016x}",
            "heads": outputs}


def scan(model, data, window=0, stride=0, whole=False, early_exit=False, ungated=False, bias=None, queries=()):
    """The windows of one input and every head's outputs in each, like pb_scan (spec 12)."""
    data = bytes(data)
    if model.stream:
        return scan_stream(model, data, window, whole, ungated, bias)
    size = window or model.window
    prefix = model.query_prefix_bytes(list(queries)) if model.query_region else b""
    first = len(prefix)
    span = size - first  # document bytes per window; the stride walks them
    step = stride or (span - span // 4 if window else model.stride)
    spans = [(0, len(data))] if whole and data else ([] if whole else windows(len(data), span, step, model.min_length))
    out = []
    for start, length in spans:
        H = model.forward(prefix + data[start:start + length], early_exit)
        if H is None:
            out.append({"start": start, "length": length, "flags": 2, "label": 0, "p_positive": 0.0, "heads": []})
            continue
        result = window_result(model, H, first, start - first, bias, ungated)
        out.append(dict(start=start, length=length, **result))
    return out


def scan_stream(model, data, window, whole, ungated, bias):
    """The stream context (spec 12.4): one forward over the whole input, the heads on every chunk."""
    size = len(data)
    if size == 0 or size < model.min_length:
        return []
    chunk = size if whole else (window or model.window)
    H = model.forward(data)
    out = []
    for start in range(0, size, chunk):
        length = min(chunk, size - start)
        result = window_result(model, H[start:start + length], 0, start, bias, ungated)
        out.append(dict(start=start, length=length, **result))
    return out


def read_inputs(path):
    """The inputs file of tests/gen/random_models.py: u32 count, then u32 length + bytes per input."""
    with open(path, "rb") as f:
        blob = f.read()
    (count,) = struct.unpack_from("<I", blob, 0)
    at, out = 4, []
    for _ in range(count):
        (n,) = struct.unpack_from("<I", blob, at)
        out.append(blob[at + 4:at + 4 + n])
        at += 4 + n
    return out
