"""The operations of the numerics contract (spec/FORMAT.md, section 5), in NumPy.

Everything is float32 unless a comment says otherwise. NumPy evaluates `a * b + c` on float32 arrays with two
roundings, exactly as the contract's "separate multiply and add"; the fused multiply-add is emulated exactly.
"""
import numpy as np

F32 = np.float32
F64 = np.float64
ZERO = F32(0.0)
ONE = F32(1.0)


def f32(x):
    return np.asarray(x, dtype=F32)


def fma(a, b, c):
    """IEEE fusedMultiplyAdd on float32 (a * b + c rounded once), element-wise with broadcasting.

    The product of two floats is exact in float64. The float64 sum is then rounded to ODD (its last bit set when it
    is inexact) and rounded to float32: that double rounding gives the correctly rounded result (Boldo and Melquiond,
    2008). The error of the float64 sum comes from Knuth's TwoSum.
    """
    p = f32(a).astype(F64) * f32(b).astype(F64)
    c = f32(c).astype(F64)
    p, c = np.broadcast_arrays(p, c)
    s = p + c
    z = s - p
    err = (p - (s - z)) + (c - z)
    bits = np.ascontiguousarray(s).view(np.uint64)
    fix = (err != 0) & ((bits & np.uint64(1)) == 0) & np.isfinite(s)
    away = (err > 0) == (s > 0)
    stepped = np.where(away, bits + np.uint64(1), bits - np.uint64(1))
    s = np.where(fix, stepped.view(F64), s)
    return s.astype(F32)


def hsum8(lanes):
    """((l0 + l4) + (l1 + l5)) + ((l2 + l6) + (l3 + l7)) over the last axis (8 lanes)."""
    l = [lanes[..., i] for i in range(8)]
    return ((l[0] + l[4]) + (l[1] + l[5])) + ((l[2] + l[6]) + (l[3] + l[7]))


def dot(a, x):
    """The canonical dot product over the last axis, with broadcasting: 8 fused lanes, the hsum8 tree, then the
    remaining elements with a separate multiply and add."""
    a, x = f32(a), f32(x)
    n = a.shape[-1]
    n8 = n - n % 8
    shape = np.broadcast_shapes(a.shape[:-1], x.shape[:-1])
    lanes = np.zeros(shape + (8,), dtype=F32)
    for i in range(0, n8, 8):
        lanes = fma(a[..., i:i + 8], x[..., i:i + 8], lanes)
    r = hsum8(lanes)
    for i in range(n8, n):
        r = r + a[..., i] * x[..., i]
    return f32(r)


def matvec(W, X):
    """Y[t, o] = dot(W[o], X[t]) for W [out, in] and X [T, in]."""
    return dot(W[None, :, :], X[:, None, :])


def seqsum(init, terms, axis=-1):
    """init + terms[0] + terms[1] + ... in that order (float32), along `axis`."""
    terms = np.moveaxis(f32(terms), axis, -1)
    init = np.broadcast_to(f32(init), terms.shape[:-1])[..., None]
    return np.cumsum(np.concatenate([init, terms], axis=-1), axis=-1, dtype=F32)[..., -1]


def rms_inv(X):
    """1 / sqrt(mean(x^2) + 1e-5) of every row (last axis): the sum of squares in float64, in element order."""
    X = f32(X)
    squares = X.astype(F64) * X.astype(F64)
    q = np.cumsum(squares, axis=-1)[..., -1]
    mean = (q / X.shape[-1]).astype(F32)
    return ONE / np.sqrt(mean + F32(1e-5))


def norm(X, w):
    """RMSNorm of every row: (x * r) * w, two roundings."""
    r = rms_inv(X)
    return (f32(X) * r[..., None]) * f32(w)


def _max_ps(a, b):
    return np.where(a > b, a, b)


def _min_ps(a, b):
    return np.where(a < b, a, b)


def exp_poly(x):
    """The Cephes polynomial exp of the blocks (spec 5)."""
    x = f32(x)
    x = _min_ps(_max_ps(x, F32(-88.3762626647949)), F32(88.3762626647949))
    fx = np.floor(fma(x, F32(1.44269504088896341), F32(0.5)))
    x = fma(-fx, F32(0.693359375), x)
    x = fma(-fx, F32(-2.12194440e-4), x)
    z = x * x
    y = np.full_like(x, F32(1.9875691500e-4))
    for c in (1.3981999507e-3, 8.3334519073e-3, 4.1665795894e-2, 1.6666665459e-1, 5.0000001201e-1):
        y = fma(y, x, F32(c))
    y = fma(y, z, x + ONE)
    exponent = ((fx.astype(np.int64) + 127) & 0xFFFFFFFF) << 23
    scale = (exponent & 0xFFFFFFFF).astype(np.uint32).view(F32)
    return y * scale


def silu_poly(x):
    x = f32(x)
    return x / (ONE + exp_poly(ZERO - x))


# The C library's binary32 functions. They are computed here in float64 and rounded once, which is the correctly
# rounded result in almost every case; C libraries may differ by a few units in the last place (spec 5).
def expf(x):
    return np.exp(f32(x).astype(F64)).astype(F32)


def logf(x):
    return np.log(f32(x).astype(F64)).astype(F32)


def log1pf(x):
    return np.log1p(f32(x).astype(F64)).astype(F32)


def sinf(x):
    return np.sin(f32(x).astype(F64)).astype(F32)


def cosf(x):
    return np.cos(f32(x).astype(F64)).astype(F32)


def sigmoid(v):
    return ONE / (ONE + expf(-f32(v)))


def softplus(v):
    v = f32(v)
    with np.errstate(over="ignore"):  # the branch not taken may overflow
        return np.where(v > F32(20.0), v, log1pf(expf(v)))


def fmax(a, b):
    """C fmax: a NaN operand loses."""
    return np.fmax(f32(a), f32(b))
