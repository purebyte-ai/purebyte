"""python -m purebyte_ref MODEL INPUTS [options]: the reference's version of tests/tools/model_dump (same JSON).

Options: --window=N --stride=N --whole --early-exit --ungated --bias=X --type-bias=X,Y,... --query=TEXT --hidden
INPUTS is either the inputs file of tests/gen/random_models.py or, with --raw, one file scanned as one input.
"""
import json
import sys

import numpy as np

from .model import Model
from .runtime import read_inputs, scan


def encode_head(out):
    if out is None:
        return None
    return {
        "label": int(out.get("label", -1)),
        "values": [float(v) for v in np.asarray(out["values"], dtype=np.float32)],
        "spans": [[int(s[0]), int(s[1]), int(s[2]), float(s[3])] for s in out.get("spans", [])],
        "byte_map": bytes(out["byte_map"]).hex() if "byte_map" in out else "",
    }


def dump(model, inputs, window=0, stride=0, whole=False, early_exit=False, ungated=False, bias=None, queries=(),
         hidden=False):
    result = {"kernel": "reference", "inputs": []}
    for data in inputs:
        entry = {"size": len(data), "windows": []}
        for w in scan(model, data, window, stride, whole, early_exit, ungated, bias, queries):
            w = dict(w)
            w["heads"] = [encode_head(h) for h in w["heads"]]
            entry["windows"].append(w)
        if hidden and data:
            entry["hidden"] = model.forward(data).tolist()
        result["inputs"].append(entry)
    return result


def main(argv):
    if len(argv) < 3:
        raise SystemExit(__doc__)
    options = {"queries": []}
    bias = {}
    raw = False
    for arg in argv[3:]:
        key, _, value = arg.partition("=")
        if key == "--window":
            options["window"] = int(value)
        elif key == "--stride":
            options["stride"] = int(value)
        elif key in ("--whole", "--early-exit", "--ungated", "--hidden"):
            options[key[2:].replace("-", "_")] = True
        elif key == "--bias":
            bias["scalar"] = float(value)
        elif key == "--type-bias":
            bias["per_type"] = [float(v) for v in value.split(",")]
        elif key == "--query":
            options["queries"].append(value)
        elif key == "--raw":
            raw = True
        else:
            raise SystemExit(f"unknown option {arg}")
    model = Model.open(argv[1])
    if raw:
        with open(argv[2], "rb") as f:
            inputs = [f.read()]
    else:
        inputs = read_inputs(argv[2])
    json.dump(dump(model, inputs, bias=bias or None, **options), sys.stdout)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main(sys.argv)
