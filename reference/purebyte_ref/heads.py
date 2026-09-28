"""Heads (spec/FORMAT.md, section 11). Each head's run(H, first) returns a dict with some of: label, values, spans,
byte_map; the exit head has score(X) instead."""
import numpy as np

from .numerics import F32, F64, ONE, ZERO, expf, f32, fmax, logf, seqsum, sigmoid


def head_key(g, i, key, default=None):
    return g.get(f"purebyte.head.{i}.{key}", default)


# ------------------------------------------------------------------------------------------ pooling and MLP (11.1)
def pool(pooling, H):
    T = H.shape[0]

    def mean():
        return seqsum(np.zeros(H.shape[1], F32), (H / F32(T)).T)

    def maximum():
        m = np.full(H.shape[1], F32(-1e30))
        for t in range(T):
            m = fmax(m, H[t])
        return m

    if pooling == "last":
        return H[T - 1]
    if pooling == "mean":
        return mean()
    if pooling == "max":
        return maximum()
    if pooling == "last_mean":
        return np.concatenate([H[T - 1], mean()])
    if pooling == "max_mean":
        return np.concatenate([maximum(), mean()])
    raise ValueError(pooling)


class Mlp:
    def __init__(self, g, prefix):
        self.w0, self.b0 = g.tensors[prefix + "mlp.0.weight"], g.tensors[prefix + "mlp.0.bias"]
        self.w2, self.b2 = g.tensors[prefix + "mlp.2.weight"], g.tensors[prefix + "mlp.2.bias"]

    def __call__(self, x):
        h = seqsum(self.b0, self.w0 * x[None, :])
        h = h / (ONE + expf(-h))
        return seqsum(self.b2, self.w2 * h[None, :])


class Head:
    def __init__(self, g, i, kind):
        self.index = i
        self.type = kind
        self.gated_by = int(head_key(g, i, "gated_by", -1))
        self.labels = list(head_key(g, i, "labels", []))

    def positive(self, out):
        return False


class Choice(Head):
    def __init__(self, g, i):
        super().__init__(g, i, "choice")
        self.pooling = head_key(g, i, "pooling", "last_mean")
        self.mlp = Mlp(g, f"heads.{i}.")
        self.temperature = F32(head_key(g, i, "temperature", 1.0))

    def run(self, H, first):
        v = self.mlp(pool(self.pooling, H)) / self.temperature
        m = F32(-1e30)
        for x in v:
            m = fmax(m, x)
        e = expf(v - m)
        p = e / seqsum(ZERO, e)
        return {"values": p, "label": int(np.argmax(p))}

    def positive(self, out):
        return out["label"] != 0


class MultiLabel(Head):
    def __init__(self, g, i):
        super().__init__(g, i, "multilabel")
        self.pooling = head_key(g, i, "pooling", "last_mean")
        self.mlp = Mlp(g, f"heads.{i}.")
        self.temperature = F32(head_key(g, i, "temperature", 1.0))
        n = self.mlp.b2.shape[0]
        self.thresholds = f32(head_key(g, i, "thresholds", [0.5] * n))

    def run(self, H, first):
        return {"values": sigmoid(self.mlp(pool(self.pooling, H)) / self.temperature), "label": -1}

    def positive(self, out):
        return bool(np.any(out["values"] >= self.thresholds))


class Score(Head):
    def __init__(self, g, i):
        super().__init__(g, i, "score")
        self.pooling = head_key(g, i, "pooling", "last_mean")
        self.mlp = Mlp(g, f"heads.{i}.")
        n = self.mlp.b2.shape[0]
        self.scale = f32(head_key(g, i, "scale", [1.0] * n))
        self.offset = f32(head_key(g, i, "offset", [0.0] * n))

    def run(self, H, first):
        return {"values": self.mlp(pool(self.pooling, H)) * self.scale + self.offset, "label": -1}


class Ordinal(Head):
    def __init__(self, g, i):
        super().__init__(g, i, "ordinal")
        self.pooling = head_key(g, i, "pooling", "last_mean")
        self.w = g.tensors[f"heads.{i}.w.weight"][0]
        self.b = np.sort(g.tensors[f"heads.{i}.b"])[::-1]

    def run(self, H, first):
        z = seqsum(ZERO, self.w * pool(self.pooling, H))
        p = sigmoid(z + self.b)
        return {"values": p, "label": int(np.sum(p > F32(0.5)))}

    def positive(self, out):
        return out["label"] > 0


# -------------------------------------------------------------------------------------------------- tag (11.6)
def can_start(label):
    return label == 0 or (label - 1) % 4 in (0, 3)


def can_end(label):
    return label == 0 or (label - 1) % 4 in (2, 3)


def transitions(L, entities):
    t = np.full((L, L), -1e30)
    for src in range(L):
        if can_end(src):
            for dst in range(L):
                if can_start(dst):
                    t[src, dst] = 0.0
    for e in range(entities):
        B, I, E = 1 + 4 * e, 2 + 4 * e, 3 + 4 * e
        t[B, I] = t[B, E] = t[I, I] = t[I, E] = 0.0
    return t


def viterbi(logp, bias, entities):
    T, L = logp.shape
    trans = transitions(L, entities)
    lp = logp.astype(F64)
    b = bias.astype(F64)
    start = np.array([0.0 if can_start(k) else -1e30 for k in range(L)])
    end = np.array([0.0 if can_end(k) else -1e30 for k in range(L)])
    dp = (lp[0] + b) + start
    back = np.zeros((T, L), dtype=np.int64)
    for t in range(1, T):
        cand = dp[:, None] + trans            # cand[j, k]
        arg = np.argmax(cand, axis=0)         # the first (lowest j) maximum
        back[t] = arg
        dp = (cand[arg, np.arange(L)] + lp[t]) + b
    path = np.zeros(T, dtype=np.int64)
    path[T - 1] = int(np.argmax(dp + end))
    for t in range(T - 1, 0, -1):
        path[t - 1] = back[t, path[t]]
    return path


def spans(path, logp):
    out = []

    def confidence(a, z):
        total = 0.0
        for t in range(a, z):
            total += float(np.exp(np.float64(logp[t, path[t]])))
        return float(F32(total / (z - a)))

    start, entity = -1, -1
    for t, label in enumerate(path):
        if label == 0:
            start = -1
            continue
        e, kind = (label - 1) // 4, (label - 1) % 4
        if kind == 0:
            start, entity = t, e
        elif kind == 3:
            out.append([t, t + 1, e, confidence(t, t + 1)])
            start = -1
        elif kind == 2 and start >= 0 and entity == e:
            out.append([start, t + 1, e, confidence(start, t + 1)])
            start = -1
    return out


class Tag(Head):
    def __init__(self, g, i):
        super().__init__(g, i, "tag")
        self.w = g.tensors[f"heads.{i}.proj.weight"]
        self.b = g.tensors[f"heads.{i}.proj.bias"]
        self.L = self.w.shape[0]
        self.entities = (self.L - 1) // 4
        self.operating_bias = F32(head_key(g, i, "operating_bias", 0.0))
        self.operating_bias_vec = f32(head_key(g, i, "operating_bias_vec", []))

    def label_bias(self, user):
        """user: {"per_type": [...]} or {"scalar": x} or {} (spec 11.6, operating bias)."""
        bias = np.zeros(self.L, dtype=F32)
        for e in range(self.entities):
            if user.get("per_type") is not None:
                v = user["per_type"][e]
            elif user.get("scalar") is not None:
                v = user["scalar"]
            elif self.operating_bias_vec.size:
                v = self.operating_bias_vec[e]
            else:
                v = self.operating_bias
            bias[1 + 4 * e:5 + 4 * e] = F32(v)
        return bias

    def logp(self, H):
        a = seqsum(self.b[None, :], self.w[None, :, :] * H[:, None, :])   # a[t, l]
        m = np.full(H.shape[0], F32(-1e30))
        for l in range(self.L):
            m = fmax(m, a[:, l])
        S = seqsum(ZERO, expf(a - m[:, None]))
        return a - (m + logf(S))[:, None]

    def run(self, H, first, user_bias=None):
        logp = self.logp(H)
        path = viterbi(logp, self.label_bias(user_bias or {}), self.entities)
        kept = []
        for s in spans(path, logp):
            if s[1] <= first:
                continue
            s[0] = max(s[0], first)
            kept.append(s)
        return {"values": np.zeros(0, F32), "label": -1, "spans": kept}


class ByteMap(Head):
    def __init__(self, g, i):
        super().__init__(g, i, "byte_map")
        self.w = g.tensors[f"heads.{i}.proj.weight"]
        self.b = g.tensors[f"heads.{i}.proj.bias"]

    def run(self, H, first):
        a = seqsum(self.b[None, :], self.w[None, :, :] * H[first:, None, :])
        return {"values": np.zeros(0, F32), "label": -1, "byte_map": np.argmax(a, axis=1).astype(np.uint8)}


class Exit(Head):
    def __init__(self, g, i):
        super().__init__(g, i, "exit")
        self.layer = int(head_key(g, i, "layer"))
        self.threshold = F32(head_key(g, i, "threshold"))
        self.w = g.tensors[f"heads.{i}.proj.weight"][0]
        self.b = g.tensors[f"heads.{i}.proj.bias"][0]

    def score(self, X):
        T, d = X.shape
        q = np.cumsum(X.astype(F64) ** 2, axis=1)[:, -1]
        r = ONE / np.sqrt((q / d).astype(F32) + F32(1e-5))
        v = X * r[:, None]
        f = np.full(d, -np.inf, dtype=F32)
        for t in range(T):
            f = fmax(f, v[t])
        mean = (np.cumsum(v.astype(F64), axis=0)[-1] / T).astype(F32)
        features = np.concatenate([f, mean]).astype(F64)
        return float(np.cumsum(np.concatenate([[F64(self.b)], self.w.astype(F64) * features]))[-1])

    def run(self, H, first):
        return None


HEAD_TYPES = {"choice": Choice, "multilabel": MultiLabel, "score": Score, "ordinal": Ordinal, "tag": Tag,
              "byte_map": ByteMap, "exit": Exit}
