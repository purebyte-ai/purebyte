"""Blocks of the body (spec/FORMAT.md, section 9): ssm_v2 and attention. Each transforms X [T, d] and returns it."""
import math

import numpy as np

from .numerics import (F32, ONE, ZERO, cosf, dot, exp_poly, expf, f32, fma, hsum8, matvec, norm, seqsum,
                       silu_poly, sinf, softplus)


def projection(g, name, out, inp):
    """A linear map [out, in] (spec 7): ternary (`.packed` + `.scales`) or float."""
    if name + ".packed" in g.tensors:
        group = int(g.require("purebyte.tern.group"))
        packed = g.tensors[name + ".packed"]
        codes = np.zeros((out, packed.shape[1] * 4), dtype=np.int8)
        for j in range(4):
            bits = (packed >> (2 * j)) & 3
            codes[:, j::4] = np.select([bits == 1, bits == 2], [1, -1], 0)
        scales = g.tensors[name + ".scales"]
        return codes[:, :inp].astype(F32) * np.repeat(scales, group, axis=1)
    return g.tensors[name]


def block_key(g, i, key, default=None):
    """A per-block value (purebyte.block.<i>.<key>) overrides the model-wide one (purebyte.<key>)."""
    own = f"purebyte.block.{i}.{key}"
    if own in g.metadata:
        return g.metadata[own]
    return g.metadata.get("purebyte." + key, default)


class SSM:
    """Block type ssm_v2 (spec 9.1)."""

    def __init__(self, g, i, d):
        p = f"blocks.{i}."
        self.H, self.P, self.N, self.G = (int(block_key(g, i, k)) for k in ("n_heads", "d_head", "d_state", "n_groups"))
        self.K = int(block_key(g, i, "conv_width"))
        self.direction = block_key(g, i, "direction", "causal")
        H, P, N, G = self.H, self.P, self.N, self.G
        self.di = H * P
        self.cw = self.di + 2 * G * N
        self.norm_w = g.tensors[p + "norm.weight"]
        self.in_proj = projection(g, p + "in_proj.weight", 2 * self.di + 2 * G * N, d)
        self.out_proj = projection(g, p + "out_proj.weight", d, self.di)
        self.dt_proj = g.tensors[p + "dt_proj.weight"]
        self.dt_bias = g.tensors[p + "dt_bias"]
        self.a = -expf(g.tensors[p + "A_log"])
        self.D = g.tensors[p + "D"]
        self.gated_norm = g.tensors[p + "gated_norm.weight"]
        if self.K > 0:
            self.conv_w = g.tensors[p + "conv1d.weight"].reshape(self.cw, self.K)
            self.conv_b = g.tensors[p + "conv1d.bias"]

    def conv(self, xbc):
        """Causal depthwise convolution + silu_poly; taps before row 0 are skipped (not zero)."""
        if self.K == 0:
            return silu_poly(xbc)
        T = xbc.shape[0]
        acc = np.broadcast_to(self.conv_b, xbc.shape).copy()
        for t in range(T):
            for k in range(self.K):
                src = t - (self.K - 1) + k
                if src >= 0:
                    acc[t] = fma(self.conv_w[:, k], xbc[src], acc[t])
        return silu_poly(acc)

    def scan(self, u, dt, dA, include_D):
        """The selective scan of every head and channel over the rows of u [T, cw]; returns y [T, di]."""
        H, P, N, G = self.H, self.P, self.N, self.G
        T = u.shape[0]
        x = u[:, :self.di].reshape(T, H, P)
        group = np.arange(H) // (H // G)
        B = u[:, self.di:self.di + G * N].reshape(T, G, N)[:, group]            # [T, H, N]
        C = u[:, self.di + G * N:self.di + 2 * G * N].reshape(T, G, N)[:, group]
        N8 = N - N % 8
        s = np.zeros((H, P, N), dtype=F32)
        y = np.zeros((T, H, P), dtype=F32)
        for t in range(T):
            dx = dt[t][:, None] * x[t]                                            # [H, P]
            dAt = dA[t][:, None, None]
            s[:, :, :N8] = fma(dx[:, :, None], B[t][:, None, :N8], dAt * s[:, :, :N8])
            lanes = np.zeros((H, P, 8), dtype=F32)
            for j in range(0, N8, 8):
                lanes = fma(s[:, :, j:j + 8], C[t][:, None, j:j + 8], lanes)
            o = hsum8(lanes)
            for n in range(N8, N):
                s[:, :, n] = dAt[:, :, 0] * s[:, :, n] + dx * B[t][:, None, n]
                o = o + s[:, :, n] * C[t][:, None, n]
            y[t] = o + (self.D[:, None] * x[t] if include_D else ZERO)
        return y.reshape(T, self.di)

    def forward(self, X):
        T = X.shape[0]
        di = self.di
        xn = norm(X, self.norm_w)
        proj = matvec(self.in_proj, xn)
        z, xbc = proj[:, :di], proj[:, di:di + self.cw]
        dt = softplus(self.dt_bias[None, :] + matvec(self.dt_proj, xn))
        dA = expf(self.a[None, :] * dt)
        u = self.conv(xbc)
        if self.direction == "causal":
            y = self.scan(u, dt, dA, True)
        else:
            hydra = self.direction == "hydra"
            ub = self.conv(xbc[::-1].copy())
            yf = self.scan(u, dt, dA, not hydra)
            yb = self.scan(ub, dt[::-1].copy(), dA[::-1].copy(), not hydra)
            if hydra:
                f = np.concatenate([np.zeros((1, di), F32), yf[:-1]])        # yf[t-1], 0 at t = 0
                b = np.concatenate([yb[:-1][::-1], np.zeros((1, di), F32)])  # yb[T-2-t], 0 at t = T-1
                heads = np.repeat(self.D, self.P)
                y = (f + b) + heads[None, :] * u[:, :di]
            else:
                y = yf + yb[::-1]
        y = y * (z / (ONE + exp_poly(ZERO - z)))
        gs = di // self.G
        y = np.concatenate([norm(y[:, g * gs:(g + 1) * gs], self.gated_norm[g * gs:(g + 1) * gs]) for g in range(self.G)], axis=1)
        return X + matvec(self.out_proj, y)


class Attention:
    """Block type attention (spec 9.2)."""

    def __init__(self, g, i, d):
        p = f"blocks.{i}."
        self.hd = int(g.get(f"purebyte.block.{i}.head_dim", 64))
        self.heads = int(g.get(f"purebyte.block.{i}.n_heads", d // self.hd))
        self.d = d
        self.norm_w = g.tensors[p + "norm.weight"]
        self.qkv = projection(g, p + "qkv.weight", 3 * d, d)
        self.out = projection(g, p + "out.weight", d, d)
        half = self.hd // 2
        step = F32(-math.log(10000.0) / half)
        self.freq = expf(np.arange(half, dtype=F32) * step)

    def rope(self, v):
        """Rotate-half RoPE of v [T, heads, hd] at positions 0..T-1."""
        half = self.hd // 2
        angle = np.arange(v.shape[0], dtype=F32)[:, None] * self.freq[None, :]
        c, s = cosf(angle)[:, None, :], sinf(angle)[:, None, :]
        x1, x2 = v[..., :half], v[..., half:]
        return np.concatenate([x1 * c - x2 * s, x1 * s + x2 * c], axis=-1)

    def forward(self, X):
        T, d, hd = X.shape[0], self.d, self.hd
        qkv = matvec(self.qkv, norm(X, self.norm_w))
        q = self.rope(qkv[:, :d].reshape(T, self.heads, hd))
        k = self.rope(qkv[:, d:2 * d].reshape(T, self.heads, hd))
        v = qkv[:, 2 * d:].reshape(T, self.heads, hd)
        scale = ONE / np.sqrt(F32(hd))
        y = np.zeros((T, self.heads, hd), dtype=F32)
        for h in range(self.heads):
            e = dot(k[None, :, h, :], q[:, None, h, :]) * scale                   # e[t, i] = k_i . q_t
            for t in range(T):
                m = np.max(e[t, :t + 1])
                w = expf(e[t, :t + 1] - m)
                S = seqsum(ZERO, w)
                y[t, h] = seqsum(np.zeros(hd, F32), ((w / S)[:, None] * v[:t + 1, h, :]).T)
        return X + matvec(self.out, y.reshape(T, d))


BLOCK_TYPES = {"ssm_v2": SSM, "attention": Attention}
