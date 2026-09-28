"""Hashed byte n-gram tables (spec/FORMAT.md, section 8.1)."""
import re

import numpy as np

from .numerics import F32, dot, matvec, rms_inv, sigmoid

M = 2**31 - 1


class NGram:
    def __init__(self, g, d):
        tables = {}
        for name in g.tensors:
            m = re.fullmatch(r"ngram\.(\d+)\.(?:(\d+)\.)?table", name)
            if m:
                tables.setdefault(int(m.group(1)), []).append(name)
        self.orders = sorted(tables)
        self.heads = int(g.get("purebyte.ngram.heads", 1))
        self.bits = int(g.get("purebyte.ngram.bits", 32))
        self.layer = int(g.get("purebyte.ngram.layer", 0))
        self.gated = bool(g.get("purebyte.ngram.gate", False))
        self.gate_bias = F32(g.tensors["ngram.gate_bias"].reshape(-1)[0]) if self.gated else None
        self.d = d
        self.rows = []  # per (order, head): a function row index -> float32 row
        for order in self.orders:
            for h in range(self.heads):
                base = f"ngram.{order}." + (f"{h}." if self.heads > 1 else "")
                table = g.tensors[base + "table"]
                if self.bits == 4:
                    low = (table & 0xF).astype(np.int8)
                    high = (table >> 4).astype(np.int8)
                    codes = np.stack([np.where(low >= 8, low - 16, low), np.where(high >= 8, high - 16, high)], axis=-1)
                    values = codes.reshape(table.shape[0], -1).astype(F32) * g.tensors[base + "scale"][:, None]
                    self.rows.append(values)
                else:
                    self.rows.append(table)
        self.buckets = self.rows[0].shape[0]
        self.proj = g.tensors["ngram.proj.weight"]

    def row_indices(self, data, start, T):
        """Rows selected for positions start .. start+T-1 of `data`; bytes before index 0 of `data` count as 0."""
        out = np.zeros((T, len(self.orders) * self.heads), dtype=np.int64)
        for t in range(T):
            pos = start + t
            column = 0
            for i, n in enumerate(self.orders):
                for h in range(self.heads):
                    v = (1000 * i + 7 + 7919 * h) % M
                    for back in range(n - 1, -1, -1):
                        p = pos - back
                        c = data[p] if p >= 0 else 0
                        v = (v * 257 + c + 1) % M
                    out[t, column] = v % self.buckets
                    column += 1
        return out

    def apply(self, X, data, start):
        """Adds the n-gram vectors of positions start .. start+T-1 of `data` to X [T, d]."""
        T = X.shape[0]
        rows = self.row_indices(data, start, T)
        cat = np.concatenate([self.rows[k][rows[:, k]] for k in range(rows.shape[1])], axis=1)
        e = matvec(self.proj, cat)
        if not self.gated:
            return X + e
        rx, re_ = rms_inv(X), rms_inv(e)
        s = dot(X * rx[:, None], e * re_[:, None])
        a = sigmoid(s / np.sqrt(F32(self.d)) + self.gate_bias)
        return X + a[:, None] * e
