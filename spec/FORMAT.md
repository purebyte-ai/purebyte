# PureByte model format

**Format version 3.** This document is normative: the runtime in `engine/` and the NumPy reference in `reference/`
implement it, and the test suite generates its random models from it. It describes how a model file is read and how
the model it declares is **executed**. How a model is trained is out of scope.

The key words MUST, MUST NOT, SHOULD and MAY are used as in RFC 2119.

- [1. Overview](#1-overview)
- [2. Container](#2-container)
- [3. Versioning and compatibility](#3-versioning-and-compatibility)
- [4. Model metadata](#4-model-metadata)
- [5. Numerics](#5-numerics)
- [6. The forward pass](#6-the-forward-pass)
- [7. Projections](#7-projections)
- [8. Input modules](#8-input-modules)
- [9. Blocks](#9-blocks)
- [10. Lookahead](#10-lookahead)
- [11. Heads](#11-heads)
- [12. Running a model over an input](#12-running-a-model-over-an-input)
- [13. Validation](#13-validation)
- [Appendix A. Metadata keys](#appendix-a-metadata-keys)
- [Appendix B. Change log](#appendix-b-change-log)

## 1. Overview

A PureByte model reads raw bytes. It is one GGUF file that declares everything the runtime needs:

```
bytes ─► byte embedding (+ n-gram tables, at a declared layer)
      ─► blocks, in order (block registry: ssm_v2, attention)
      ─► final RMSNorm (─► optional lookahead)
      ─► heads (head registry: choice, multilabel, score, ordinal, tag, byte_map, exit)
```

and how to run it over inputs of any length: windows of a declared size and stride, or one stream with carried state
(§12). The runtime knows nothing about the task: a model that finds credentials and a model that labels file regions
differ only in their files. A file that uses anything this document does not define is refused (§13).

Conventions:

- Shapes are written in row-major order, `[rows, columns]`, as PyTorch writes them. GGUF stores dimensions in the
  opposite order (fastest-varying first): a tensor of shape `[out, in]` has GGUF dimensions `(in, out)`.
- `d` is `purebyte.d_model`. `T` is the number of positions (bytes) in a window. Positions are 0-based.
- Integers in formulas are exact; `float` means IEEE 754 binary32 and `double` binary64. §5 defines every operation.
- A metadata key written `purebyte.head.<i>.name` stands for `purebyte.head.0.name`, `purebyte.head.1.name`, ...;
  tensor names follow the same pattern (`blocks.<i>.norm.weight`).

## 2. Container

The file is a GGUF file, version 3 (<https://github.com/ggml-org/ggml/blob/master/docs/gguf.md>), little-endian:

| Field | Type | Value |
|---|---|---|
| magic | u32 | `0x46554747` ("GGUF") |
| version | u32 | 3 |
| tensor count | u64 | at most 2^20 |
| metadata count | u64 | at most 2^20 |
| metadata | key-value pairs | strings are a u64 length and bytes; value types are the 13 GGUF types |
| tensor infos | name, rank, dimensions, type, offset | rank at most 4; every dimension at least 1 |
| padding | | to the alignment |
| tensor data | | each tensor at `data_start + offset` |

Rules on top of GGUF:

- `general.alignment` MAY be present; it MUST then be a power of two within 8 and 65536. The default is 32. Every
  tensor offset MUST be a multiple of the alignment, and every tensor MUST lie inside the file. No two tensors may
  share a byte: overlapping tensor data is refused.
- Two tensor types are used: **F32** (GGML type 0) for every number, and **I8** (GGML type 24) for packed data
  (ternary codes and 4-bit n-gram tables), read as raw bytes. Any other type is refused.
- A metadata key or a tensor name MUST NOT appear twice. A file MUST NOT hold more than 2^20 metadata values in total
  (array items included), and arrays MUST NOT nest deeper than 4 levels.
- Booleans are 0 or 1. Where this document asks for an integer, any GGUF integer type is accepted; where it asks for a
  number, integers and floats are accepted.
- Readers treat the file as untrusted: every length, count, offset and dimension is checked before use.

## 3. Versioning and compatibility

`purebyte.format_version` (integer, required) is the version of this document the file follows.

| Version | Content |
|---|---|
| 2 | The first exported models: `ssm_v2` blocks, 32-bit or 4-bit n-gram tables with one hash head added after the embedding, `choice`, `tag` and `exit` heads, ternary projections. Window policy by default only. |
| 3 | This document: `attention` blocks, lookahead, n-gram hash heads, gate and injection layer, `multilabel`, `score`, `ordinal` and `byte_map` heads, head gating, aggregation and pooling keys, the window policy, the stream context, query templates and the declared profile. |

Compatibility rules:

1. A reader of version *v* reads every file of version 2 to *v*. A file of a newer version is refused with
   `PB_ERR_UNSUPPORTED`; a file without a version, or with a version below 2, with `PB_ERR_FORMAT`.
2. A writer SHOULD write the lowest version that describes its file.
3. `purebyte.required_features` (array of strings, optional) lists capabilities the file needs. A reader refuses a
   file that requires a feature it does not know. Version 3 defines: `attention`, `lookahead`, `ngram_gate`,
   `ngram_layer`, `ngram_heads`, `multilabel`, `score`, `ordinal`, `byte_map`, `query`, `stream`. Future optional
   capabilities of the same format version get new names here.
4. Unknown metadata keys are ignored. A key that changes what the model computes MUST therefore come with a new format
   version or a required feature. Keys that exporters write for their own purposes (for example
   `purebyte.chunk_size`) do not affect execution.
5. Every tensor in the file MUST be used by the model it declares. A tensor the reader does not use means the file
   computes something the reader would not: it is refused with `PB_ERR_UNSUPPORTED`.

## 4. Model metadata

| Key | Type | Required | Meaning |
|---|---|---|---|
| `general.architecture` | string | yes | `purebyte` |
| `general.name` | string | no | a display name |
| `general.version`, `general.license`, `general.license.name` | string | no | informative: the model's version, and its license as an SPDX identifier (or `other`) with a display name; not read by the runtime |
| `purebyte.format_version` | integer | yes | §3 |
| `purebyte.required_features` | string array | no | §3 |
| `purebyte.d_model` | integer 1..65536 | yes | width `d` of the residual stream |
| `purebyte.n_layers` | integer 1..4096 | yes | number of blocks |
| `purebyte.block.<i>.type` | string | no | block type of block *i* (default `ssm_v2`), §9 |
| `purebyte.head.count` | integer 0..64 | no | number of heads (default 1) |
| `purebyte.head.<i>.type` | string | no | head type of head *i* (default `choice`), §11 |
| `purebyte.tern.type` | string | no | `pb_tern2` (the only one), §7 |
| `purebyte.tern.encoding` | string | no | `00=0,01=+1,10=-1` (the only one), §7 |
| `purebyte.tern.group` | integer | when a projection is ternary | inputs per ternary scale, §7 |
| `purebyte.window.size` | integer 1..2^20 | no | window length in bytes (default 512), §12 |
| `purebyte.window.stride` | integer 1..span | no | advance between windows (default `span - span / 4`, integer division), where `span` is the document bytes of a window: `size`, or `size - query.region` with a query template (§12.3) |
| `purebyte.window.min` | integer 1..span | no | shortest window evaluated, in document bytes (default 24, or `span` when smaller); in the stream context, the shortest input (1..2^24), §12.4 |
| `purebyte.context` | string | no | `window` (default) or `stream`, §12.4 |
| `purebyte.query.*` | | no | query template, §12.3 |
| `purebyte.profile` | string | no | post-processing the model is meant for (`none`, `secrets-code`, `secrets-binary`, `redact`); see [OUTPUT.md](OUTPUT.md) |

Every tensor and key of the parts is listed with the part. Appendix A lists all keys.

The model-wide tensors are:

| Tensor | Shape | Meaning |
|---|---|---|
| `embed.weight` | `[256, d]` | row *b* is the vector of byte value *b* |
| `out_norm.weight` | `[d]` | weight of the final RMSNorm |

## 5. Numerics

The runtime promises the same bits from every kernel on one platform: the portable scalar kernel, AVX2 and NEON
compute the same IEEE operations in the same order. This section is the order. Operations not listed here are
ordinary binary32 operations (`+`, `-`, `*`, `/` rounded to nearest even) evaluated exactly as written, left to right;
implementations MUST NOT contract `a * b + c` into a fused multiply-add or reassociate sums (C and C++ compilers:
`-ffp-contract=off`, no fast-math, `/fp:precise`).

**Fused multiply-add.** `fma(a, b, c)` is the IEEE 754 fusedMultiplyAdd on binary32: `a * b + c` rounded once.
Where no hardware instruction exists it can be emulated exactly: `a * b` is exact in binary64; add `c` in binary64,
round that sum to odd, then round to binary32.

**Canonical dot product.** `dot(a, x, n)` of two float vectors:

```
lane[0..7] = 0
for i in 0 .. n8 - 1  (n8 = n - n mod 8):  lane[i mod 8] = fma(a[i], x[i], lane[i mod 8])
r = ((lane[0] + lane[4]) + (lane[1] + lane[5])) + ((lane[2] + lane[6]) + (lane[3] + lane[7]))
for i in n8 .. n - 1:  r = r + a[i] * x[i]          (the product rounded, then the sum rounded)
```

Every matrix-vector product of the blocks and of the n-gram module (`y[o] = dot(W[o], x, in)`) is a canonical dot.

**Sequential sum.** `seqsum(b; w, x)`: `s = b; for i ascending: s = s + w[i] * x[i]` with the product rounded before
the sum. Heads use it (§11).

**RMS normalization.** For a row `x` of `n` floats:

```
q = sum over i ascending of double(x[i]) * double(x[i])        (in double, element order)
r = 1.0f / sqrtf(float(q / n) + 1e-5f)                           (q / n in double, then rounded to float)
norm(x, w)[i] = (x[i] * r) * w[i]                                (two roundings, in that order)
```

**Exponential and SiLU inside blocks.** `exp_poly(x)` is the Cephes single-precision polynomial:

```
x  = min(max(x, -88.3762626647949f), 88.3762626647949f)      (x86 MAXPS/MINPS semantics)
fx = floor(fma(x, 1.44269504088896341f, 0.5f))
x  = fma(-fx, 0.693359375f, x)
x  = fma(-fx, -2.12194440e-4f, x)
z  = x * x
y  = 1.9875691500e-4f
y  = fma(y, x, 1.3981999507e-3f);  y = fma(y, x, 8.3334519073e-3f);  y = fma(y, x, 4.1665795894e-2f)
y  = fma(y, x, 1.6666665459e-1f);  y = fma(y, x, 5.0000001201e-1f)
y  = fma(y, z, x + 1.0f)
exp_poly = y * s                      s: the float whose bits are (int(fx) + 127) << 23. The clamp keeps int(fx)
                                      within -127..127: s is 2^int(fx) for -126..127, and +0.0 for -127 (so
                                      exp_poly is 0 there, not the subnormal exp(x))
silu_poly(x) = x / (1.0f + exp_poly(0.0f - x))
```

**Library functions.** Everywhere else `expf`, `logf`, `log1pf`, `sqrtf`, `sinf` and `cosf` are the platform's binary32
functions. `sqrtf` is correctly rounded everywhere; the others may differ by a few units in the last place (ULP)
between C libraries. `sigmoid(v) = 1.0f / (1.0f + expf(-v))`, `softplus(v) = v if v > 20 else log1pf(expf(v))`.
`fmax` is the C function (a NaN operand loses).

**Guarantees.** On one platform, every kernel gives the same bits. Across platforms, results agree up to the ULP
differences of the library functions, amplified by the layers: decisions and spans are expected to agree, raw values
to a relative difference of about 1e-4 or less. The test suite compares accordingly (tests/README.md).

## 6. The forward pass

For a window of bytes `b[0..T-1]` (the runtime chooses windows as described in §12):

```
X[t] = embed.weight[b[t]]                                        for t in 0..T-1
if n-grams and purebyte.ngram.layer == 0:  X = X + ngram(b)      (§8.1)
for l in 0 .. n_layers - 1:
    if n-grams and purebyte.ngram.layer == l > 0:  X = X + ngram(b)
    X = block_l(X)                                               (§9)
    if early exit is on and the exit head's layer == l + 1:  stop the window if exit_score(X) < threshold  (§11.8)
H[t] = norm(X[t], out_norm.weight)
if lookahead:  H = lookahead(H)                                  (§10)
heads(H)                                                         (§11)
```

`X` is the residual stream, `[T, d]` floats. Blocks transform it in place and add their output to it.

## 7. Projections

A linear map `y = W x` with `W` of shape `[out, in]` is stored in one of two forms; the loader picks the form by the
tensors present.

- **Float:** tensor `<name>` F32 `[out, in]`.
- **Ternary (`pb_tern2`):** tensors `<name>.packed` I8 `[out, ceil(in / 4)]` and `<name>.scales` F32
  `[out, in / group]`, with `group = purebyte.tern.group`, `group > 0` and `in mod group == 0`. Code `j` of row `o` is
  in byte `j / 4` of the row, bits `2 * (j mod 4)` and `2 * (j mod 4) + 1`: `00` = 0, `01` = +1, `10` = −1; `11` is
  invalid, and the padding codes of the last byte of a row (`j >= in`) MUST be `00`. The weight is
  `W[o][j] = float(code) * scales[o][j / group]` (exact).

In both forms `y[o] = dot(W[o], x, in)` (§5).

## 8. Input modules

### 8.1 N-gram tables

Hashed tables of byte n-grams add a vector to the residual stream before block `purebyte.ngram.layer`. They are
present when the file has tables; the orders are the orders of the tables.

Tensors, for each order `n` (a decimal number) and, with several hash heads, each head `h`:

| Tensor | Shape | Meaning |
|---|---|---|
| `ngram.<n>.table` (one hash head) or `ngram.<n>.<h>.table` | F32 `[buckets, part]`, or I8 `[buckets, part / 2]` with 4-bit codes | the table |
| `ngram.<n>.scale` or `ngram.<n>.<h>.scale` | F32 `[buckets]` | 4-bit tables only: one scale per row |
| `ngram.proj.weight` | F32 `[d, width]` | projection of the concatenated rows, `width = orders × heads × part` |
| `ngram.gate_bias` | F32 scalar (`[]` or `[1]`) | with the gate only |

All tables have the same shape. A table has at most 2^24 buckets, and the concatenated rows (`width`) at most 65,536
columns; a file over these limits is refused. Keys:

| Key | Type | Default | Meaning |
|---|---|---|---|
| `purebyte.ngram.orders` | integer array | | if present, MUST equal the ascending list of table orders (an empty list: no tables) |
| `purebyte.ngram.heads` | integer 1..64 | 1 | hash heads per order |
| `purebyte.ngram.bits` | 4 or 32 | 32 | table precision |
| `purebyte.ngram.hash` | string | | if present, MUST be `poly base=257 mod=2^31-1 seed=1000*i+7` (one head) or `poly base=257 mod=2^31-1 seed=1000*i+7+7919*h` |
| `purebyte.ngram.buckets`, `purebyte.ngram.dim` | integer | | if present and not 0, MUST equal `buckets` and `part × heads` |
| `purebyte.ngram.layer` | integer 0..n_layers-1 | 0 | the vector is added before this block (0: right after the embedding) |
| `purebyte.ngram.gate` | boolean | false | scale the vector by a context gate |

Orders are within 1..64. For position `t` the module computes, for every order index `i` (0 for the smallest order,
ascending) and hash head `h`:

```
v = (1000 * i + 7 + 7919 * h) mod M                   M = 2^31 - 1
for k = n_i - 1 down to 0:                              the n_i bytes ending at t, oldest first
    c = b[t - k] if the position t - k is readable, else 0
    v = (v * 257 + c + 1) mod M
row = v mod buckets
```

In a window, positions before the window's first byte are not readable (they count as 0) even when the input has
bytes there; in a stream (§12.4) the earlier bytes of the stream are readable. The selected rows are concatenated,
order-major then head, into `c[0..width-1]`. A 4-bit row holds `part / 2` bytes; byte `j` gives columns `2j` (low
nibble) and `2j + 1` (high nibble), each nibble a two's-complement integer in −8..7, and the column value is
`scale[row] * float(nibble)`. Then:

```
e[o] = dot(ngram.proj.weight[o], c, width)                            o in 0..d-1
without the gate:  X[t][o] = X[t][o] + e[o]
with the gate:     rx = r(X[t]), re = r(e)      (r: the RMS factor of §5)
                   s  = dot(X[t][·] * rx, e[·] * re, d)        (each element rounded after its multiplication)
                   a  = sigmoid(s / sqrtf(float(d)) + gate_bias)
                   X[t][o] = X[t][o] + a * e[o]
```

## 9. Blocks

`purebyte.block.<i>.type` selects the block type of block *i* (default `ssm_v2`). Tensors of block *i* are named
`blocks.<i>.*`.

### 9.1 `ssm_v2`

A Mamba-2 style selective state-space block. Hyper-parameters are read from `purebyte.block.<i>.<key>` when present,
else from the model-wide `purebyte.<key>`:

| Key | Symbol | Range |
|---|---|---|
| `n_heads` | H | 1..4096 |
| `d_head` | P | 1..4096 |
| `d_state` | N | 1..4096 |
| `n_groups` | G | 1..4096, H mod G = 0 |
| `conv_width` | K | 0..64 (0: no convolution) |
| `direction` | | `causal` (default), `bimamba` or `hydra` |

With `di = H × P` (inner width), `cw = di + 2 G N` (convolved channels) and `po = 2 di + 2 G N`:

| Tensor | Shape |
|---|---|
| `norm.weight` | `[d]` |
| `in_proj.weight` (projection, §7) | `[po, d]` |
| `dt_proj.weight` | `[H, d]` |
| `dt_bias`, `A_log`, `D` | `[H]` each |
| `conv1d.weight`, `conv1d.bias` | `[cw, 1, K]` and `[cw]`, only when K > 0 |
| `gated_norm.weight` | `[di]` |
| `out_proj.weight` (projection, §7) | `[d, di]` |

Computation over a window of `T` positions (each step for every position before the next step):

1. `xn[t] = norm(X[t], norm.weight)`.
2. `p[t] = in_proj · xn[t]` (`po` values): `z = p[t][0 .. di-1]`, `xbc = p[t][di .. di+cw-1]`.
3. For each head `h`: `dt[t][h] = softplus(dt_bias[h] + dot(dt_proj[h], xn[t], d))` and
   `dA[t][h] = expf(a[h] * dt[t][h])` with `a[h] = -expf(A_log[h])`.
4. Convolution and SiLU of the `cw` channels: `u[t][c] = silu_poly(acc)` where
   `acc = conv1d.bias[c]` and then, for `k = 0 .. K-1` ascending, `acc = fma(w[c][k], xbc[t-(K-1)+k][c], acc)`,
   skipping taps whose position is before the first readable row (in a window, row 0). With K = 0,
   `u[t][c] = silu_poly(xbc[t][c])`. `u` splits into `x = u[·][0 .. di-1]`, `B_g = u[·][di + gN .. di + gN + N-1]` and
   `C_g = u[·][di + GN + gN .. di + GN + gN + N-1]` for group `g`.
5. Selective scan, per head `h` (group `g = h / (H / G)`) and channel `c` of that head, with a state `s[0..N-1]` that
   starts at 0 and is carried over positions `t = 0, 1, ...`:

   ```
   dx = dt[t][h] * x[t][c]
   lane[0..7] = 0;  N8 = N - N mod 8
   for n in 0 .. N8-1:  s[n] = fma(dx, B_g[t][n], dA[t][h] * s[n]);  lane[n mod 8] = fma(s[n], C_g[t][n], lane[n mod 8])
   o = ((lane[0] + lane[4]) + (lane[1] + lane[5])) + ((lane[2] + lane[6]) + (lane[3] + lane[7]))
   for n in N8 .. N-1:  s[n] = dA[t][h] * s[n] + dx * B_g[t][n];  o = o + s[n] * C_g[t][n]
   y[t][c] = o + D[h] * x[t][c]
   ```

   (In the `n >= N8` loop every product is rounded before its sum.) `hydra` omits the `D` term here (step 5b).
   **Directions.** `causal` is the scan above. `bimamba` and `hydra` also run steps 4–5 on the reversed sequence:
   `xbc` rows and `dt`, `dA` values in reverse order, convolved from their own first row. With `yf` the forward
   result and `yb` the reversed one (row `r` of `yb` belongs to position `T-1-r`):
   - `bimamba`: `y[t] = yf[t] + yb[T-1-t]` (both computed with the `D` term);
   - `hydra` (5b): `y[t][c] = (f + b) + D[h] * x[t][c]` with `f = yf[t-1][c]` (0 for t = 0) and `b = yb[T-2-t][c]`
     (0 for t = T-1), both scans without the `D` term, and `x` the forward convolution output.
6. Gate: `y[t][c] = y[t][c] * (z[t][c] / (1.0f + exp_poly(0.0f - z[t][c])))`.
7. Group norm: for each group `g` of `di / G` consecutive channels, `y[t][g] = norm(y[t][g], gated_norm.weight[g])`.
8. `X[t][o] = X[t][o] + dot(out_proj[o], y[t], di)`.

Only `causal` blocks can run in the stream context: their memory is the scan state and the last `K - 1` rows of `xbc`.

### 9.2 `attention`

Causal softmax self-attention with rotary position embeddings.

| Key | Default | Meaning |
|---|---|---|
| `purebyte.block.<i>.head_dim` | 64 | even, 2..4096 |
| `purebyte.block.<i>.n_heads` | `d / head_dim` | `n_heads × head_dim` MUST equal `d` |

| Tensor | Shape |
|---|---|
| `norm.weight` | `[d]` |
| `qkv.weight` (projection, §7) | `[3d, d]`: rows `0..d-1` give q, `d..2d-1` k, `2d..3d-1` v |
| `out.weight` (projection, §7) | `[d, d]` |

With `hd = head_dim`, `half = hd / 2`, head `j` of a vector being its elements `j·hd .. (j+1)·hd - 1`:

1. `xn[t] = norm(X[t], norm.weight)`; `q, k, v = qkv · xn[t]`.
2. RoPE on q and k of every head, rotate-half: `step = float(-ln(10000) / half)` (computed in double, then rounded),
   `f_j = expf(float(j) * step)` for `j < half`; at position `t`, `θ = float(t) * f_j`, `c = cosf(θ)`,
   `s = sinf(θ)`, and `(v_j, v_{j+half}) ← (v_j * c - v_{j+half} * s, v_j * s + v_{j+half} * c)` (each product
   rounded, then the difference or sum).
3. For head `j` and position `t`: `e_i = dot(k_i, q_t, hd) * (1.0f / sqrtf(float(hd)))` for `i = 0..t`;
   `m = max_i e_i`; `w_i = expf(e_i - m)`; `S = w_0 + w_1 + ... + w_t` (sequential);
   `y_t = 0`, then for `i = 0..t` ascending: `y_t[c] = y_t[c] + (w_i / S) * v_i[c]`.
4. `X[t][o] = X[t][o] + dot(out[o], y[t], d)`.

An attention block needs the whole window: it cannot run in the stream context.

## 10. Lookahead

Optional: tensor `look.conv_w` F32 `[d, k]` (1 ≤ k ≤ 1024); `purebyte.lookahead` (integer), if present, MUST equal
`k`. Applied to the final hidden states, it gives every head the `k` bytes to the right of each position:

```
acc = w[c][0] * H[t+1][c];  for j = 2..k:  acc = acc + w[c][j-1] * H[t+j][c]      (H past the window's end is 0)
H'[t][c] = H[t][c] + acc
```

A model with a lookahead cannot run in the stream context.

## 11. Heads

`purebyte.head.count` heads (default 1) are numbered from 0; tensors of head *i* are named `heads.<i>.*`. A file
without `purebyte.head.count` has one head of type `choice`. Keys common to all heads:

| Key | Default | Meaning |
|---|---|---|
| `purebyte.head.<i>.type` | `choice` | head type |
| `purebyte.head.<i>.name` | the type | display name; a name an earlier head has gets `_<i>` appended, again until no earlier head has it (`tag`, `tag_2`, `tag` give `tag`, `tag_2`, `tag_2_2`) |
| `purebyte.head.<i>.labels` | none | names of the classes, labels, outputs, levels or entity types; if present, the count MUST match the head |
| `purebyte.head.<i>.gated_by` | −1 | −1 (not gated) or the index of a head: run this head only in windows where head `gated_by` decides positive (§11.9) |
| `purebyte.head.<i>.aggregate` | per type | window-level heads (§11.1): how window values combine over a document; see [OUTPUT.md](OUTPUT.md). The outputs of `tag` and `byte_map` heads are never aggregated: a reader ignores the key on them, and a writer SHOULD omit it there |

### 11.1 Pooling and the window MLP

Window-level heads (`choice`, `multilabel`, `score`, `ordinal`) pool the `T` hidden rows, the rows of a query prefix
included, into one vector. `purebyte.head.<i>.pooling`:

| Value | Vector | Computation |
|---|---|---|
| `last` | `d` | `H[T-1]` |
| `mean` | `d` | `m = 0`, then for t ascending `m[j] = m[j] + H[t][j] / float(T)` |
| `max` | `d` | `m = -1e30f`, then for t ascending `m[j] = fmax(m[j], H[t][j])` |
| `last_mean` (default) | `2d` | `[last | mean]` |
| `max_mean` | `2d` | `[max | mean]` |

`choice`, `multilabel` and `score` then apply an MLP: tensors `mlp.0.weight` `[hidden, pooled]`, `mlp.0.bias`
`[hidden]`, `mlp.2.weight` `[n, hidden]`, `mlp.2.bias` `[n]` (widths from the shapes):

```
g[o]   = seqsum(mlp.0.bias[o]; mlp.0.weight[o], pooled);   g[o] = g[o] / (1.0f + expf(-g[o]))
out[o] = seqsum(mlp.2.bias[o]; mlp.2.weight[o], g)
```

### 11.2 `choice`

One decision among `n` classes; class 0 is the negative class. `purebyte.head.<i>.temperature` (number > 0, default 1).

```
v[k] = out[k] / temperature;  m = fmax over k of v[k], starting from -1e30f
e[k] = expf(v[k] - m);  S = e[0] + e[1] + ...;  p[k] = e[k] / S
label = the first k with the largest p[k]
```

Outputs: `p` (n values) and `label`. Positive (as a gate, and for the window's `p_positive = 1 - p[0]`): `label != 0`.

Every weight is a finite number (§13), but a computation can still overflow (logits beyond the float range give NaN
probabilities). A value that is not a finite number is never positive, here and in every head: comparisons with NaN
are false, so a NaN probability is never the largest (`label` stays 0 when every value is NaN), never reaches a
threshold (§11.3) and never counts above 0.5 (§11.5).
Aggregates: `max` (default), `mean`, `vote`, `any`.

### 11.3 `multilabel`

`n` independent yes/no labels: `p[k] = sigmoid(out[k] / temperature)`. `purebyte.head.<i>.thresholds` (n numbers,
default 0.5 each): label k is on when `p[k] >= threshold[k]`. Positive when any label is on. Aggregates: `max`
(default), `mean`, `any`.

### 11.4 `score`

`n` real values: `value[k] = out[k] * scale[k] + offset[k]` (product rounded first), with
`purebyte.head.<i>.scale` and `.offset` (n numbers each, defaults 1 and 0). Aggregates: `mean` (default), `max`,
`min`.

### 11.5 `ordinal`

A level among `L` ordered levels. Tensors `w.weight` `[1, pooled]` (no MLP, no bias) and `b` `[L-1]`. The thresholds
`b` are sorted in decreasing order when the model is loaded (they are finite numbers, as every tensor value, §13).

```
z = seqsum(0; w.weight[0], pooled)
p[k] = sigmoid(z + b[k])   for k = 0..L-2        (the probability that the level is above k)
level = number of k with p[k] > 0.5
```

Outputs: `p` (L−1 values) and `level`. Positive when `level > 0`. Aggregates: `max` (default), `mean`.

### 11.6 `tag`

Typed spans over the window's bytes with the BIOES scheme for `k` entity types. A model has at most one `tag` head
(a result has one list of spans). Tensors `proj.weight` `[L, d]` and `proj.bias` `[L]` with `L = 1 + 4k`,
`1 <= k <= 256`. Keys: `purebyte.head.<i>.n_entities` and `.n_labels` (if present, MUST
equal `k` and `L`), `.scheme` (if present, MUST be `BIOES:O,B-e,I-e,E-e,S-e`), `.labels` (k entity names; default
`entity_0`, ...), `.operating_bias` (number, default 0), `.operating_bias_vec` (k numbers).

Labels: 0 = O; for entity `e`: B = 1 + 4e, I = 2 + 4e, E = 3 + 4e, S = 4 + 4e. Per position:

```
a[l] = seqsum(proj.bias[l]; proj.weight[l], H[t])
m = fmax over l of a[l], starting from -1e30f;  S = sum over l ascending of expf(a[l] - m)
logp[t][l] = a[l] - (m + logf(S))
```

**Operating bias.** One value per entity type, added to its four labels (never to O). The first that applies: the
caller's per-type vector, the caller's single value, the file's `operating_bias_vec`, the file's `operating_bias`.

**Decoding.** Viterbi in double precision: a sequence starts with O, B or S and ends with O, E or S; after O, E-x or
S-x come O, B-y or S-y; after B-x or I-x come I-x or E-x (the same entity). The score of a sequence is the sum of
`double(logp[t][l_t]) + double(bias[l_t])`, plus −1e30 for each start, end or step that breaks these rules (a
penalty, not an exclusion: such a sequence wins only when every valid one scores below about −1e30, which extreme
operating biases can cause). At every step the best predecessor is the one with the highest score above −1e300 (a
NaN score is never the highest), the lowest label index on ties, label 0 when none is above −1e300; the last label is
chosen the same way. A step costs O(L): the score of a transition depends only on the kinds of its two labels.

**Spans.** Walking the decoded labels: B opens a span; E of the same entity closes it (inclusive); S is a one-byte
span; O discards an open span. A span's confidence is the mean over its bytes of `exp(double(logp[t][l_t]))`, without
the bias, rounded to float. Spans that end inside a query prefix are dropped, and spans that start inside it are cut
to its end.

### 11.7 `byte_map`

One class per byte: tensors `proj.weight` `[C, d]` and `proj.bias` `[C]`, 2 ≤ C ≤ 256.
`a[c] = seqsum(proj.bias[c]; proj.weight[c], H[t])`; the byte's label is the first `c` with the largest `a[c]`. Bytes
of a query prefix get no label.

### 11.8 `exit`

An early-exit probe on the residual stream after `layer` blocks. Keys (all required): `purebyte.head.<i>.features`
= `rms_max_mean`, `.layer` (1..n_layers−1), `.threshold` (number). Tensors `proj.weight` `[1, 2d]` and `proj.bias`
`[1]`. At most one exit head per model; it produces no output.

```
for every position t:  q = sum over j ascending of double(X[t][j])^2;  r = 1.0f / sqrtf(float(q / d) + 1e-5f)
                       v[j] = X[t][j] * r
f[j]     = max over t of v[j]  (fmax, starting from -infinity)
f[d + j] = float((sum over t ascending of double(v[j])) / T)
score = double(bias) + sum over j ascending of double(w[j]) * double(f[j])       (in double)
```

When the caller enables early exit, a window whose `score < double(threshold)` stops after block `layer - 1` and
counts as negative: no other head runs. Early exit is an approximation the caller opts into; it is off by default.

### 11.9 Gating

A head with `gated_by = g` runs only in windows where head `g` is positive. Head `g` MUST be an ungated `choice`,
`multilabel` or `ordinal` head other than the head itself. Ungated heads run first, in index order, then the gated
ones. A caller MAY ask the runtime to run gated heads regardless (`PB_SCAN_UNGATED`).

## 12. Running a model over an input

### 12.1 Windows

With the window context (the default), an input of `n` bytes is cut into windows of `size` bytes starting at
`0, stride, 2 × stride, ...`; each window is `min(size, n - start)` bytes long. The walk stops after the first window
that reaches the end of the input. When the next window would be shorter than `window.min` bytes, it is not evaluated
and one last window of `size` bytes aligned to the end, starting at `max(0, n - size)`, is evaluated instead, so that
every byte of the input is in some window. An input shorter than `window.min` has no window. Each window is an
independent forward pass (§6); nothing is carried between windows. Callers MAY override the size and stride (results
then differ from the declared behavior; a stride longer than a window's document bytes, or windows of fewer document
bytes than `window.min`, are refused) or run an input as one window.

With `size - stride >= window.min - 1` no walk ever needs the last aligned window: the next window always holds at
least `size - stride + 1` bytes. The released models (512, 384, and 24 or 1) are such.

### 12.2 Window outputs

Per window the runtime reports every head's outputs (§11), the label and `p_positive` of the first `choice` head,
and whether the window was gated, stopped by the exit head, or skipped by an optional prefilter of the caller.
Offsets of spans are made absolute (input coordinates). How windows combine into document results is defined in
[OUTPUT.md](OUTPUT.md).

A window's **digest** is the 64-bit FNV-1a hash (offset basis `0xcbf29ce484222325`, prime `0x100000001b3`) of the
bytes of its final hidden states (`T × d` little-endian floats, the lookahead output when there is one) followed by
the bytes of the float values of every computed head, in head order (the probabilities, scores and cumulative
probabilities of window-level heads; tag spans and byte labels, which are not floats, are not part of it). It
fingerprints the numerics of one window for reproducibility checks.

### 12.3 Query template

Query-conditioned models extract fields named at run time. `purebyte.query.region` (integer 1..2^20, smaller than
the window size) enables the template; `purebyte.query.max` (integer 1..1024, required with it) bounds the number of
queries; `purebyte.query.prefix` (default `?`), `.suffix` (default a line feed) and `.pad` (one byte, default a line
feed) shape it. Every window is:

```
[ prefix + query_1 + suffix + prefix + query_2 + suffix + ... padded with `pad` to `region` bytes ] [ document bytes ]
```

holding `span = size - region` document bytes; windows advance over the document by the stride, which counts
document bytes (at most `span`; default `span - span / 4`), and `window.min` counts document bytes too (at most
`span`). Queries longer than the region in total are refused. Spans and byte labels are reported for document bytes
only (§11.6, §11.7).

### 12.4 Stream context

With `purebyte.context = stream`, each input is ONE sequence. The runtime runs it in consecutive chunks of `size`
bytes and carries each block's state from one chunk to the next, so that the hidden states are exactly those of a
single forward pass over the whole input, and the n-gram tables read the bytes before a chunk. The heads run on every
chunk, which is reported as a window: window-level heads pool over the chunk, and tag spans never cross a chunk
boundary. An input shorter than `window.min` has no chunk; a short last chunk is evaluated.

A model with the stream context MUST NOT have attention blocks, `ssm_v2` blocks other than `causal`, a lookahead, a
query template or an exit head. `purebyte.window.stride` does not apply to it (chunks are consecutive), and callers
cannot ask for a stride other than the chunk size, a prefilter or early exit.

## 13. Validation

A reader MUST refuse a file, naming what it refuses, when:

- it is not GGUF (wrong magic), or it is truncated or malformed (§2): `PB_ERR_FORMAT`;
- `general.architecture` is not `purebyte`, or `purebyte.format_version` is missing or below 2: `PB_ERR_FORMAT`;
- its GGUF version is not 3, the format version is newer than the reader's, a required feature, block type, head
  type, direction, pooling, label scheme, exit feature set, hash, precision, ternary type or encoding is unknown, or
  a tensor has a GGML type other than F32 (0) and I8 (24): `PB_ERR_UNSUPPORTED`;
- a tensor is missing, has another type (F32 where I8 is expected, or the reverse) or shape than this document says,
  or is not used: `PB_ERR_FORMAT` (unused tensors: `PB_ERR_UNSUPPORTED`, §3); a scalar is stored as `[]` or `[1]`;
- a tensor holds a NaN or an infinity, or a number of the metadata is not a finite number within the float range:
  `PB_ERR_FORMAT`;
- a key has the wrong type, a required key is missing, a value is outside its range (integers are compared as the
  64-bit values of the file, before any narrowing), or two declarations contradict each other (counts of labels,
  orders, dimensions, gating targets, a stride longer than the document bytes of a window or a `window.min` larger
  than them, more than one exit head or tag head, the stream context with a part that cannot stream):
  `PB_ERR_FORMAT`;
- a ternary code is `11` or a padding code is not `00`: `PB_ERR_FORMAT`;
- one window of the declared size would cost more than this document allows (below): `PB_ERR_FORMAT`.

A reader MUST NOT guess: a value it does not understand is an error, never a default.

**Limits.** The shapes of a file decide what a window costs, and a small file must not be able to declare a window
that needs gigabytes of memory or hours of work. Besides the ranges of §4 to §11, a reader refuses:

- `window.size × w > 2^24`, where `w` is the widest row a window holds per position: `d_model`, the in_proj output
  `2 × di + 2 × G × N` of every `ssm_v2` block, `3 × d_model` for every `attention` block, and `L = 1 + 4k` of the tag
  head;
- an `ssm_v2` block whose in_proj output exceeds 2^20 floats, or whose scan updates more than 2^20 floats per position
  (`n_heads × d_head × d_state`);
- a tag head of more than 256 entity types (§11.6);
- a `window.size` above 8192 in a model with `attention` blocks, whose work grows with the square of the window.

The released models are far within these limits (a window of 512 bytes with rows of at most 1,280 floats, a scan of
32,768 floats per position, one entity type).

## Appendix A. Metadata keys

Model: `general.architecture`, `general.name`, `general.alignment`, `general.version`, `general.license`,
`general.license.name`, `purebyte.format_version`, `purebyte.required_features`, `purebyte.d_model`,
`purebyte.n_layers`, `purebyte.tern.type`, `purebyte.tern.encoding`, `purebyte.tern.group`, `purebyte.window.size`,
`purebyte.window.stride`, `purebyte.window.min`, `purebyte.context`, `purebyte.profile`, `purebyte.query.region`,
`purebyte.query.max`, `purebyte.query.prefix`, `purebyte.query.suffix`, `purebyte.query.pad`, `purebyte.lookahead`.

`ssm_v2` (model-wide, or per block as `purebyte.block.<i>.<key>`): `n_heads`, `d_head`, `d_state`, `n_groups`,
`conv_width`, `direction`. Blocks: `purebyte.block.<i>.type`; attention: `purebyte.block.<i>.head_dim`,
`purebyte.block.<i>.n_heads`.

N-grams: `purebyte.ngram.orders`, `.heads`, `.bits`, `.hash`, `.buckets`, `.dim`, `.layer`, `.gate`.

Heads: `purebyte.head.count`; `purebyte.head.<i>.type`, `.name`, `.labels`, `.gated_by`, `.aggregate`, `.pooling`,
`.temperature`, `.thresholds`, `.scale`, `.offset`, `.n_entities`, `.n_labels`, `.scheme`, `.operating_bias`,
`.operating_bias_vec`, `.features`, `.layer`, `.threshold`.

## Appendix B. Change log

- **Version 3** (runtime 1.0.0): this document.
- **Version 2**: the first exported models (§3).
