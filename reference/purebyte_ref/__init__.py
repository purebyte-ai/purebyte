"""purebyte_ref: a readable NumPy implementation of the PureByte model format (spec/FORMAT.md).

It runs every block and head type of the specification, operation by operation in the order the numerics contract
fixes, and is the executable companion of the specification. It is slow on purpose (plain loops where the order of
operations matters) and is used by the test suite to check the engine.

    from purebyte_ref import Model, scan
    model = Model.open("model.gguf")
    for window in scan(model, open("input.bin", "rb").read()):
        print(window["start"], window["label"], window["heads"])
"""
from .model import Model
from .runtime import scan, windows

__all__ = ["Model", "scan", "windows"]
