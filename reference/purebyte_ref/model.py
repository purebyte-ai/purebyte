"""A model assembled from its file (spec/FORMAT.md, sections 4 and 6), and its forward pass over one sequence."""
import numpy as np

from .blocks import BLOCK_TYPES
from .gguf import GGUF, FormatError
from .heads import HEAD_TYPES, Exit
from .ngram import NGram
from .numerics import F32, norm


class Lookahead:
    """spec 10: H'[t] = H[t] + sum_j w[:, j-1] * H[t + j] (0 past the end)."""

    def __init__(self, g):
        self.w = g.tensors["look.conv_w"]  # [d, k]

    def __call__(self, H):
        T, _ = H.shape
        k = self.w.shape[1]
        padded = np.concatenate([H, np.zeros((k, H.shape[1]), F32)])
        acc = self.w[:, 0][None, :] * padded[1:T + 1]
        for j in range(2, k + 1):
            acc = acc + self.w[:, j - 1][None, :] * padded[j:T + j]
        return H + acc


class Model:
    def __init__(self, g):
        if g.get("general.architecture") != "purebyte":
            raise FormatError("not a PureByte model")
        self.version = int(g.require("purebyte.format_version"))
        self.d = d = int(g.require("purebyte.d_model"))
        n_layers = int(g.require("purebyte.n_layers"))
        self.embedding = g.tensors["embed.weight"]
        self.ngram = NGram(g, d) if any(k.startswith("ngram.") and k.endswith(".table") for k in g.tensors) else None
        self.blocks = []
        for i in range(n_layers):
            kind = g.get(f"purebyte.block.{i}.type", "ssm_v2")
            self.blocks.append(BLOCK_TYPES[kind](g, i, d))
        self.final_norm = g.tensors["out_norm.weight"]
        self.lookahead = Lookahead(g) if "look.conv_w" in g.tensors else None
        count = int(g.get("purebyte.head.count", 1))
        self.heads = [HEAD_TYPES[g.get(f"purebyte.head.{i}.type", "choice")](g, i) for i in range(count)]
        self.exit = next((h for h in self.heads if isinstance(h, Exit)), None)
        self.first_choice = next((h.index for h in self.heads if h.type == "choice"), -1)
        self.window = int(g.get("purebyte.window.size", 512))
        self.stream = g.get("purebyte.context", "window") == "stream"
        self.query_region = int(g.get("purebyte.query.region", 0))
        span = self.window - self.query_region  # document bytes per window (spec 12.1, 12.3)
        self.stride = int(g.get("purebyte.window.stride", span - span // 4))
        self.min_length = int(g.get("purebyte.window.min", 24 if self.stream else min(24, span)))
        self.query_max = int(g.get("purebyte.query.max", 0))
        self.query_prefix = g.get("purebyte.query.prefix", "?").encode("utf-8", "surrogateescape")  # the file's bytes
        self.query_suffix = g.get("purebyte.query.suffix", "\n").encode("utf-8", "surrogateescape")
        self.query_pad = g.get("purebyte.query.pad", "\n").encode("utf-8", "surrogateescape")

    @classmethod
    def open(cls, path):
        return cls(GGUF.open(path))

    def forward(self, data, early_exit=False):
        """Final hidden states [T, d] of `data` run as one sequence (spec 6), or None when the exit head stops it."""
        X = self.embedding[np.frombuffer(bytes(data), dtype=np.uint8)].astype(F32)
        if self.ngram and self.ngram.layer == 0:
            X = self.ngram.apply(X, data, 0)
        for l, block in enumerate(self.blocks):
            if self.ngram and l > 0 and self.ngram.layer == l:
                X = self.ngram.apply(X, data, 0)
            X = block.forward(X)
            if early_exit and self.exit and self.exit.layer == l + 1:
                if self.exit.score(X) < float(self.exit.threshold):
                    return None
        H = norm(X, self.final_norm)
        return self.lookahead(H) if self.lookahead else H

    def query_prefix_bytes(self, queries):
        if len(queries) > self.query_max:
            raise ValueError("too many queries")
        text = b"".join(self.query_prefix + q.encode() + self.query_suffix for q in queries)
        if len(text) > self.query_region:
            raise ValueError("the queries do not fit in the query region")
        return text + self.query_pad * (self.query_region - len(text))
