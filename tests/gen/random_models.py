"""Random tiny models written from spec/FORMAT.md, standard library only.

Every model is fully determined by its name: the weights come from a SplitMix64 generator seeded by the name, so the
files are byte-identical on every platform and the golden files can refer to them. Together the models cover every
block type and direction, every head type and pooling, every n-gram variant, the lookahead, the query template, the
stream context, gating, early exit, per-block hyper-parameters, ternary and float projections, and sizes that are not
multiples of the 8 lanes of the numerics contract.

    python random_models.py OUT_DIR        writes OUT_DIR/<name>.gguf and OUT_DIR/<name>.inputs.bin for every model
"""
import math
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gguf_writer import GGUFWriter  # noqa: E402

MASK = (1 << 64) - 1


def f32(x):
    """x rounded to the nearest float32 (the value the file will hold)."""
    return struct.unpack("<f", struct.pack("<f", x))[0]


class Rng:
    """SplitMix64: tiny, fast enough, and the same stream everywhere."""

    def __init__(self, seed):
        self.state = seed & MASK

    def next(self):
        self.state = (self.state + 0x9E3779B97F4A7C15) & MASK
        z = self.state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK
        return z ^ (z >> 31)

    def uniform(self, lo=0.0, hi=1.0):
        return lo + (hi - lo) * ((self.next() >> 11) / float(1 << 53))

    def normal(self, sigma=1.0):
        u1 = max(self.uniform(), 1e-300)
        u2 = self.uniform()
        return sigma * math.sqrt(-2.0 * math.log(u1)) * math.cos(2.0 * math.pi * u2)

    def normals(self, n, sigma=1.0, mean=0.0):
        return [f32(mean + self.normal(sigma)) for _ in range(n)]

    def uniforms(self, n, lo, hi):
        return [f32(self.uniform(lo, hi)) for _ in range(n)]

    def below(self, n):
        return self.next() % n


def pack_ternary(codes, rows, cols):
    """pb_tern2 packing (spec 7): four codes per byte, code j in bits 2*(j % 4); 00 = 0, 01 = +1, 10 = -1."""
    row_bytes = (cols + 3) // 4
    out = bytearray(rows * row_bytes)
    value = {0: 0, 1: 1, -1: 2}
    for r in range(rows):
        for j in range(cols):
            out[r * row_bytes + j // 4] |= value[codes[r * cols + j]] << (2 * (j % 4))
    return bytes(out), row_bytes


def pack_nibbles(codes):
    """4-bit n-gram rows (spec 8.1): column 2j in the low nibble, 2j + 1 in the high nibble, two's complement."""
    out = bytearray(len(codes) // 2)
    for j in range(len(out)):
        out[j] = (codes[2 * j] & 0xF) | ((codes[2 * j + 1] & 0xF) << 4)
    return bytes(out)


class Builder:
    """Collects the metadata and tensors of one model, one spec section per method."""

    def __init__(self, name, d_model, n_layers, version=3):
        self.name = name
        self.rng = Rng(zlib.crc32(name.encode()) * 0x100000001B3 + 7)
        self.w = GGUFWriter()
        self.d = d_model
        self.n_layers = n_layers
        self.w.add("general.architecture", "purebyte")
        self.w.add("general.name", name)
        self.w.add("purebyte.format_version", version)
        self.w.add("purebyte.d_model", d_model)
        self.w.add("purebyte.n_layers", n_layers)
        self.w.add_f32("embed.weight", [256, d_model], self.rng.normals(256 * d_model, 1.0))
        self.tern_group = 0
        self.heads = 0

    def meta(self, key, value):
        self.w.add(key, value)

    def norm_weight(self, name, n):
        self.w.add_f32(name, [n], self.rng.normals(n, 0.1, 1.0))

    def projection(self, name, out, inp, ternary):
        """A linear map [out, in], ternary (spec 7) or float."""
        if not ternary:
            self.w.add_f32(name, [out, inp], self.rng.normals(out * inp, 1.0 / math.sqrt(inp)))
            return
        group = self.tern_group
        assert group and inp % group == 0, "set tern_group first"
        codes = []
        for _ in range(out * inp):
            u = self.rng.below(8)
            codes.append(0 if u < 3 else (1 if u < 6 else -1))
        packed, row_bytes = pack_ternary(codes, out, inp)
        self.w.add_i8(name + ".packed", [out, row_bytes], packed)
        self.w.add_f32(name + ".scales", [out, inp // group], self.rng.uniforms(out * (inp // group), 0.4 / math.sqrt(inp), 1.6 / math.sqrt(inp)))

    def ternary(self, group):
        self.tern_group = group
        self.w.add("purebyte.tern.type", "pb_tern2")
        self.w.add("purebyte.tern.group", group)
        self.w.add("purebyte.tern.encoding", "00=0,01=+1,10=-1")

    # ------------------------------------------------------------------------------------------------ blocks (9)
    def ssm(self, i, H, P, N, G, K, ternary, direction=None):
        p = f"blocks.{i}."
        d, di = self.d, H * P
        cw = di + 2 * G * N
        self.norm_weight(p + "norm.weight", d)
        self.projection(p + "in_proj.weight", 2 * di + 2 * G * N, d, ternary)
        self.w.add_f32(p + "dt_proj.weight", [H, d], self.rng.normals(H * d, 0.3 / math.sqrt(d)))
        dts = [self.rng.uniform(0.02, 0.3) for _ in range(H)]
        self.w.add_f32(p + "dt_bias", [H], [f32(math.log(math.expm1(v))) for v in dts])
        self.w.add_f32(p + "A_log", [H], [f32(math.log(self.rng.uniform(1.0, 8.0))) for _ in range(H)])
        self.w.add_f32(p + "D", [H], self.rng.uniforms(H, 0.5, 1.5))
        if K > 0:
            self.w.add_f32(p + "conv1d.weight", [cw, 1, K], self.rng.normals(cw * K, 0.5))
            self.w.add_f32(p + "conv1d.bias", [cw], self.rng.normals(cw, 0.1))
        self.norm_weight(p + "gated_norm.weight", di)
        self.projection(p + "out_proj.weight", d, di, ternary)
        self.meta(f"purebyte.block.{i}.type", "ssm_v2")
        if direction:
            self.meta(f"purebyte.block.{i}.direction", direction)

    def ssm_defaults(self, H, P, N, G, K, direction="causal"):
        """The model-wide ssm_v2 hyper-parameters (spec 9.1)."""
        for key, value in (("n_heads", H), ("d_head", P), ("d_state", N), ("n_groups", G), ("conv_width", K)):
            self.meta("purebyte." + key, value)
        self.meta("purebyte.direction", direction)

    def ssm_override(self, i, **values):
        for key, value in values.items():
            self.meta(f"purebyte.block.{i}.{key}", value)

    def attention(self, i, head_dim, ternary, declare_heads=False):
        p = f"blocks.{i}."
        d = self.d
        self.norm_weight(p + "norm.weight", d)
        self.projection(p + "qkv.weight", 3 * d, d, ternary)
        self.projection(p + "out.weight", d, d, ternary)
        self.meta(f"purebyte.block.{i}.type", "attention")
        self.meta(f"purebyte.block.{i}.head_dim", head_dim)
        if declare_heads:
            self.meta(f"purebyte.block.{i}.n_heads", d // head_dim)

    # ------------------------------------------------------------------------------------------- input modules (8)
    def ngram(self, orders, buckets, part, heads=1, bits=32, layer=None, gate=False):
        for order in orders:
            for h in range(heads):
                base = f"ngram.{order}." + (f"{h}." if heads > 1 else "")
                if bits == 32:
                    self.w.add_f32(base + "table", [buckets, part], self.rng.normals(buckets * part, 1.0))
                else:
                    codes = [int(self.rng.below(16)) - 8 for _ in range(buckets * part)]
                    self.w.add_i8(base + "table", [buckets, part // 2], pack_nibbles(codes))
                    self.w.add_f32(base + "scale", [buckets], self.rng.uniforms(buckets, 0.05, 0.3))
        width = len(orders) * heads * part
        self.w.add_f32("ngram.proj.weight", [self.d, width], self.rng.normals(self.d * width, 1.0 / math.sqrt(width)))
        self.meta("purebyte.ngram.orders", list(orders))
        self.meta("purebyte.ngram.buckets", buckets)
        self.meta("purebyte.ngram.dim", part * heads)
        if heads > 1:
            self.meta("purebyte.ngram.heads", heads)
            self.meta("purebyte.ngram.hash", "poly base=257 mod=2^31-1 seed=1000*i+7+7919*h")
        else:
            self.meta("purebyte.ngram.hash", "poly base=257 mod=2^31-1 seed=1000*i+7")
        if bits != 32:
            self.meta("purebyte.ngram.bits", bits)
        if layer is not None:
            self.meta("purebyte.ngram.layer", layer)
        if gate:
            self.meta("purebyte.ngram.gate", True)
            self.w.add_f32("ngram.gate_bias", [], [f32(self.rng.uniform(-0.5, 0.5))])

    def final(self, lookahead=0):
        self.norm_weight("out_norm.weight", self.d)
        if lookahead:
            self.w.add_f32("look.conv_w", [self.d, lookahead], self.rng.normals(self.d * lookahead, 0.4))
            self.meta("purebyte.lookahead", lookahead)

    # --------------------------------------------------------------------------------------------------- heads (11)
    def head_keys(self, i, kind, **keys):
        self.meta(f"purebyte.head.{i}.type", kind)
        for key, value in keys.items():
            self.meta(f"purebyte.head.{i}.{key}", value)

    def pooled(self, pooling):
        return 2 * self.d if pooling in ("last_mean", "max_mean") else self.d

    def mlp(self, i, pooling, hidden, outputs, gain):
        n = self.pooled(pooling)
        p = f"heads.{i}."
        self.w.add_f32(p + "mlp.0.weight", [hidden, n], self.rng.normals(hidden * n, 1.5 / math.sqrt(n)))
        self.w.add_f32(p + "mlp.0.bias", [hidden], self.rng.normals(hidden, 0.2))
        self.w.add_f32(p + "mlp.2.weight", [outputs, hidden], self.rng.normals(outputs * hidden, gain / math.sqrt(hidden)))
        self.w.add_f32(p + "mlp.2.bias", [outputs], self.rng.normals(outputs, 0.5))

    def choice(self, i, pooling, classes, hidden=12, **keys):
        self.mlp(i, pooling, hidden, classes, 4.0)
        self.head_keys(i, "choice", pooling=pooling, **keys)

    def multilabel(self, i, pooling, labels, hidden=10, **keys):
        self.mlp(i, pooling, hidden, labels, 4.0)
        self.head_keys(i, "multilabel", pooling=pooling, **keys)

    def score(self, i, pooling, outputs, hidden=8, **keys):
        self.mlp(i, pooling, hidden, outputs, 2.0)
        self.head_keys(i, "score", pooling=pooling, **keys)

    def ordinal(self, i, pooling, levels, **keys):
        n = self.pooled(pooling)
        self.w.add_f32(f"heads.{i}.w.weight", [1, n], self.rng.normals(n, 3.0 / math.sqrt(n)))
        self.w.add_f32(f"heads.{i}.b", [levels - 1], self.rng.uniforms(levels - 1, -2.0, 2.0))  # unsorted on purpose
        self.head_keys(i, "ordinal", pooling=pooling, **keys)

    def tag(self, i, entities, **keys):
        L = 1 + 4 * entities
        self.w.add_f32(f"heads.{i}.proj.weight", [L, self.d], self.rng.normals(L * self.d, 3.0 / math.sqrt(self.d)))
        bias = self.rng.normals(L, 0.5)
        bias[0] = f32(bias[0] + 1.0)  # O a little more likely: spans of several lengths, not every byte
        self.w.add_f32(f"heads.{i}.proj.bias", [L], bias)
        self.head_keys(i, "tag", n_entities=entities, n_labels=L, scheme="BIOES:O,B-e,I-e,E-e,S-e", **keys)

    def byte_map(self, i, classes, **keys):
        self.w.add_f32(f"heads.{i}.proj.weight", [classes, self.d], self.rng.normals(classes * self.d, 2.0 / math.sqrt(self.d)))
        self.w.add_f32(f"heads.{i}.proj.bias", [classes], self.rng.normals(classes, 0.3))
        self.head_keys(i, "byte_map", **keys)

    def exit(self, i, layer, threshold):
        self.w.add_f32(f"heads.{i}.proj.weight", [1, 2 * self.d], self.rng.normals(2 * self.d, 1.0 / math.sqrt(2 * self.d)))
        self.w.add_f32(f"heads.{i}.proj.bias", [1], [0.0])
        self.head_keys(i, "exit", features="rms_max_mean", layer=layer, threshold=threshold)

    def tobytes(self):
        return self.w.tobytes()


# ------------------------------------------------------------------------------------------------------- models
def ssm_causal_ternary():
    """ssm_v2 causal with ternary projections, a per-block override, 32-bit n-grams at layer 0, a tag head gated by
    a choice head, and an exit head. Sizes are multiples of 8 (the shape of the released models)."""
    b = Builder("ssm-causal-ternary", 32, 3)
    b.ternary(8)
    b.ssm_defaults(H=4, P=8, N=16, G=2, K=4)
    b.ngram([1, 3], buckets=61, part=8)
    b.ssm(0, 4, 8, 16, 2, 4, True)
    b.ssm_override(1, d_state=8)
    b.ssm(1, 4, 8, 8, 2, 4, True)
    b.ssm(2, 4, 8, 16, 2, 4, True)
    b.final()
    b.meta("purebyte.head.count", 3)
    b.tag(0, 2, gated_by=1, labels=["alpha", "beta"], operating_bias_vec=[0.25, -0.25])
    b.choice(1, "max", 2, labels=["negative", "positive"])
    b.exit(2, 2, 1.65)  # about half of the test windows score below it
    b.meta("purebyte.window.size", 64)
    b.meta("purebyte.window.stride", 48)
    b.meta("purebyte.profile", "none")
    return b


def ssm_bidirectional():
    """bimamba and hydra blocks, sizes that are not multiples of 8 (d = 20, P = 12, N = 12), a block without
    convolution, mixed ternary and float projections; multilabel, score and a gated byte_map head."""
    b = Builder("ssm-bidirectional", 20, 2)
    b.ternary(4)
    b.ssm_defaults(H=2, P=12, N=12, G=1, K=3, direction="bimamba")
    b.ssm(0, 2, 12, 12, 1, 3, True)
    b.ssm_override(1, conv_width=0, direction="hydra")
    b.ssm(1, 2, 12, 12, 1, 0, False)
    b.final()
    b.meta("purebyte.head.count", 3)
    b.multilabel(0, "mean", 3, thresholds=[0.3, 0.5, 0.7], temperature=1.5, aggregate="mean")
    b.score(1, "last", 2, scale=[2.0, -1.0], offset=[0.5, 0.0], labels=["size", "density"])
    b.byte_map(2, 4, gated_by=0, labels=["a", "b", "c", "d"])
    b.meta("purebyte.window.size", 48)
    b.meta("purebyte.window.stride", 40)
    b.meta("purebyte.window.min", 16)
    return b


def attention_lookahead():
    """ssm_v2 then two attention blocks (declared and default head counts, ternary and float), a lookahead; choice
    with last_mean pooling and a temperature, an ungated tag head, an ordinal head with max_mean pooling."""
    b = Builder("attention-lookahead", 32, 3)
    b.ternary(8)
    b.ssm_defaults(H=2, P=16, N=8, G=1, K=4)
    b.ssm(0, 2, 16, 8, 1, 4, True)
    b.attention(1, 8, True, declare_heads=True)
    b.attention(2, 16, False)
    b.final(lookahead=3)
    b.meta("purebyte.head.count", 3)
    b.choice(0, "last_mean", 3, temperature=0.8, aggregate="vote")
    b.tag(1, 1, operating_bias=0.5)
    b.ordinal(2, "max_mean", 4, labels=["none", "low", "medium", "high"])
    b.meta("purebyte.window.size", 80)
    b.meta("purebyte.window.stride", 60)
    b.meta("purebyte.required_features", ["attention", "lookahead", "ordinal"])
    return b


def ngram_gated_4bit():
    """4-bit n-gram tables with two hash heads, injected before block 1 through the context gate."""
    b = Builder("ngram-gated-4bit", 16, 2)
    b.ssm_defaults(H=2, P=8, N=8, G=1, K=4)
    b.ngram([2, 4, 5], buckets=53, part=8, heads=2, bits=4, layer=1, gate=True)
    b.ssm(0, 2, 8, 8, 1, 4, False)
    b.ssm(1, 2, 8, 8, 1, 4, False)
    b.final()
    b.meta("purebyte.head.count", 2)
    b.choice(0, "mean", 2)
    b.tag(1, 3, gated_by=0)
    b.meta("purebyte.window.size", 64)
    b.meta("purebyte.window.stride", 64)
    b.meta("purebyte.required_features", ["ngram_gate", "ngram_layer", "ngram_heads"])
    return b


def query_extraction():
    """A query template: every window starts with the queries; tag spans and byte labels only on document bytes."""
    b = Builder("query-extraction", 24, 2)
    b.ternary(8)
    b.ssm_defaults(H=3, P=8, N=8, G=1, K=4)
    b.ngram([2], buckets=31, part=8)
    b.ssm(0, 3, 8, 8, 1, 4, True)
    b.ssm(1, 3, 8, 8, 1, 4, True)
    b.final()
    b.meta("purebyte.head.count", 2)
    b.tag(0, 2, labels=["name", "value"])
    b.byte_map(1, 3)
    b.meta("purebyte.window.size", 96)
    b.meta("purebyte.window.stride", 64)
    b.meta("purebyte.query.region", 24)
    b.meta("purebyte.query.max", 3)
    b.meta("purebyte.required_features", ["query", "byte_map"])
    return b


def stream_context():
    """The stream context: causal blocks and n-grams, run in chunks with carried state."""
    b = Builder("stream-context", 16, 2)
    b.ssm_defaults(H=2, P=8, N=8, G=1, K=4)
    b.ngram([2, 3], buckets=41, part=8)
    b.ssm(0, 2, 8, 8, 1, 4, False)
    b.ssm(1, 2, 8, 8, 1, 4, False)
    b.final()
    b.meta("purebyte.head.count", 2)
    b.choice(0, "max", 2)
    b.tag(1, 1, gated_by=0)
    b.meta("purebyte.window.size", 40)
    b.meta("purebyte.window.min", 8)
    b.meta("purebyte.context", "stream")
    b.meta("purebyte.required_features", ["stream"])
    return b


def legacy_v2():
    """Format version 2 defaults: no head count (one choice head, last_mean pooling), no window keys."""
    b = Builder("legacy-v2", 16, 1, version=2)
    b.ssm_defaults(H=2, P=8, N=8, G=1, K=4)
    b.ssm(0, 2, 8, 8, 1, 4, False)
    b.final()
    b.mlp(0, "last_mean", 8, 2, 4.0)
    return b


MODELS = {
    "ssm-causal-ternary": ssm_causal_ternary,
    "ssm-bidirectional": ssm_bidirectional,
    "attention-lookahead": attention_lookahead,
    "ngram-gated-4bit": ngram_gated_4bit,
    "query-extraction": query_extraction,
    "stream-context": stream_context,
    "legacy-v2": legacy_v2,
}

# Queries used with query-conditioned models.
QUERIES = ["user", "password"]


def inputs_for(name):
    """The test inputs of a model: lengths around the window edges, text and binary bytes."""
    rng = Rng(zlib.crc32(("inputs:" + name).encode()))
    text = b"user = admin\nlevel: 42 (default)\nThe quick brown fox jumps over the lazy dog. " * 8
    out = []
    for length in (0, 5, 23, 24, 37, 64, 65, 150, 333):
        if length % 2:
            start = int(rng.below(len(text) - length)) if length < len(text) else 0
            out.append(text[start:start + length])
        else:
            out.append(bytes(int(rng.below(256)) for _ in range(length)))
    return out


def encode_inputs(inputs):
    """Inputs file: u32 count, then per input a u32 length and the bytes."""
    return struct.pack("<I", len(inputs)) + b"".join(struct.pack("<I", len(data)) + data for data in inputs)


def write_inputs(path, inputs):
    with open(path, "wb") as f:
        f.write(encode_inputs(inputs))


def read_inputs(path):
    with open(path, "rb") as f:
        blob = f.read()
    (count,), at, out = struct.unpack_from("<I", blob, 0), 4, []
    for _ in range(count):
        (n,) = struct.unpack_from("<I", blob, at)
        out.append(blob[at + 4:at + 4 + n])
        at += 4 + n
    return out


def main(argv):
    if len(argv) != 2:
        raise SystemExit(__doc__)
    out = argv[1]
    os.makedirs(out, exist_ok=True)
    for name, build in MODELS.items():
        with open(os.path.join(out, name + ".gguf"), "wb") as f:
            f.write(build().tobytes())
        write_inputs(os.path.join(out, name + ".inputs.bin"), inputs_for(name))
    print(f"wrote {len(MODELS)} models to {out}")


if __name__ == "__main__":
    main(sys.argv)
