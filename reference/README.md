# Reference implementation

`purebyte_ref` is a small NumPy implementation of the model format ([spec/FORMAT.md](../spec/FORMAT.md)): it reads
a model file and runs it over an input the way the engine does, block by block and head by head. It exists for two
reasons:

- **to check the engine:** the test suite runs both on random models generated from the specification and requires
  the same decisions and the same values within a documented tolerance ([tests/README.md](../tests/README.md));
- **to read:** each part of the specification is a short, plain function here, with the order of floating-point
  operations the numerics contract fixes.

It is slow on purpose (Python loops where the order of operations matters) and is not meant for production use.

It checks the container (section 2 of the specification) as the engine does, and refuses the same broken files. It
does not repeat the checks of the model itself (section 13): it assumes a file the engine loads, and the `validation`
test holds the engine to section 13.

## Use

```bash
pip install numpy
cd reference
python -m purebyte_ref MODEL.gguf INPUT --raw               # every window of INPUT, as JSON
python -m purebyte_ref MODEL.gguf INPUT --raw --hidden      # plus the final hidden states of the whole input
```

```python
from purebyte_ref import Model, scan

model = Model.open("model.gguf")
for window in scan(model, open("input.bin", "rb").read()):
    print(window["start"], window["length"], window["label"], window["heads"])
```

Options of `scan`: `window`, `stride`, `whole`, `early_exit`, `ungated`, `bias` (`{"scalar": x}` or
`{"per_type": [...]}`), `queries`.

## Map

| Module | Specification |
|---|---|
| [gguf.py](purebyte_ref/gguf.py) | Section 2: the container and its checks |
| [numerics.py](purebyte_ref/numerics.py) | Section 5: fused multiply-add (emulated exactly), the canonical dot product, RMS normalization, the polynomial exp and SiLU |
| [ngram.py](purebyte_ref/ngram.py) | Section 8.1: hashed n-gram tables, 4-bit rows, the context gate |
| [blocks.py](purebyte_ref/blocks.py) | Sections 7 and 9: ternary projections, `ssm_v2` (causal, bimamba, hydra), `attention` |
| [heads.py](purebyte_ref/heads.py) | Section 11: pooling, the window MLP, every head type, BIOES Viterbi decoding and spans |
| [model.py](purebyte_ref/model.py) | Sections 4, 6 and 10: model assembly, the forward pass, the lookahead |
| [runtime.py](purebyte_ref/runtime.py) | Section 12: windows, the query template, the stream context, gating, digests |

The C library functions of the specification (`expf`, `logf`, `log1pf`, `sinf`, `cosf`) are computed in double
precision and rounded once, which is the correctly rounded result in almost every case. C libraries are allowed to
differ from it by a few units in the last place, which is why the engine and this implementation agree within a
tolerance rather than bit for bit.
