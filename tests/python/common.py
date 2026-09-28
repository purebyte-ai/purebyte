"""Shared helpers of the Python tests: paths, the model generator, the engine's model_dump, the NumPy reference, and
the comparison rules of tests/README.md (decisions exactly, values with a tolerance)."""
import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tests", "gen"))
sys.path.insert(0, os.path.join(ROOT, "reference"))

import random_models  # noqa: E402

SKIP = 77  # the exit status ctest reports as "skipped"

# Tolerances between two implementations (engine vs reference, or engine vs a golden file made on another platform).
# They absorb the few-ULP differences of the C libraries' expf/logf/log1pf/sinf/cosf (spec/FORMAT.md, section 5).
VALUE_ATOL, VALUE_RTOL = 1e-5, 1e-4    # head values: probabilities, scores, span confidences
HIDDEN_ATOL, HIDDEN_RTOL = 1e-4, 1e-3  # final hidden states

# Scan variants every model runs with (model_dump / reference options). Query models get their queries added.
VARIANTS = {
    "default": [],
    "bias": ["--bias=0.75"],
    "ungated": ["--ungated"],
    "resized": ["--window=40", "--stride=25"],
    "whole": ["--whole"],
}
EXTRA_VARIANTS = {
    "ssm-causal-ternary": {"early-exit": ["--early-exit"], "type-bias": ["--type-bias=-0.5,1.0"]},
    "stream-context": {"chunked": ["--window=17"]},
}


def arguments(description):
    p = argparse.ArgumentParser(description=description)
    p.add_argument("--model-dump", required=True, help="path of the model_dump tool")
    p.add_argument("--work", required=True, help="scratch directory (generated models, outputs)")
    p.add_argument("--model", action="append", help="only these models (repeatable)")
    return p.parse_args()


def generate_models(work):
    """Writes the random models of tests/gen/random_models.py into work/models (idempotent) and returns the dir."""
    out = os.path.join(work, "models")
    os.makedirs(out, exist_ok=True)
    for name, build in random_models.MODELS.items():
        write_if_changed(os.path.join(out, name + ".gguf"), build().tobytes())
        write_if_changed(os.path.join(out, name + ".inputs.bin"), random_models.encode_inputs(random_models.inputs_for(name)))
    return out


def write_if_changed(path, data):
    """Tests may run in parallel: a file another test may be reading is only rewritten when it has to change."""
    if os.path.exists(path):
        with open(path, "rb") as f:
            if f.read() == data:
                return
    with open(path, "wb") as f:
        f.write(data)


def variants_for(name):
    out = dict(VARIANTS)
    out.update(EXTRA_VARIANTS.get(name, {}))
    if name == "query-extraction":
        out["resized"] = ["--window=60", "--stride=25"]  # 36 document bytes after the 24-byte query region
        out = {k: v + [f"--query={q}" for q in random_models.QUERIES] for k, v in out.items()}
    if name == "stream-context":
        out.pop("resized")  # a stream has no stride
    return out


def model_dump(tool, model, inputs, options):
    done = subprocess.run([tool, model, inputs, *options], capture_output=True)
    if done.returncode != 0:
        raise RuntimeError(f"model_dump failed: {done.stderr.decode(errors='replace').strip()}")
    return json.loads(done.stdout)


def reference_dump(model, inputs, options):
    from purebyte_ref.__main__ import dump
    from purebyte_ref.model import Model
    from purebyte_ref.runtime import read_inputs
    kwargs, bias = {"queries": []}, {}
    for arg in options:
        key, _, value = arg.partition("=")
        if key in ("--window", "--stride"):
            kwargs[key[2:]] = int(value)
        elif key in ("--whole", "--early-exit", "--ungated", "--hidden"):
            kwargs[key[2:].replace("-", "_")] = True
        elif key == "--bias":
            bias["scalar"] = float(value)
        elif key == "--type-bias":
            bias["per_type"] = [float(v) for v in value.split(",")]
        elif key == "--query":
            kwargs["queries"].append(value)
    return json.loads(json.dumps(dump(Model.open(model), read_inputs(inputs), bias=bias or None, **kwargs)))


def close(a, b, atol, rtol):
    return abs(a - b) <= atol + rtol * abs(b)


class Comparison:
    """Compares two dumps (same JSON as tests/tools/model_dump.cpp) and collects the differences."""

    def __init__(self, label):
        self.label = label
        self.problems = []
        self.windows = 0
        self.same_digests = 0
        self.max_value_error = 0.0

    def problem(self, where, what):
        if len(self.problems) < 20:
            self.problems.append(f"{self.label}: {where}: {what}")

    def values(self, where, a, b, atol, rtol):
        if len(a) != len(b):
            self.problem(where, f"{len(a)} values, expected {len(b)}")
            return
        for k, (x, y) in enumerate(zip(a, b)):
            self.max_value_error = max(self.max_value_error, abs(x - y) / (atol + rtol * abs(y)))
            if not close(x, y, atol, rtol):
                self.problem(where, f"value {k}: {x!r}, expected {y!r}")
                return

    def head(self, where, a, b):
        if (a is None) != (b is None):
            self.problem(where, f"computed: {a is not None}, expected {b is not None}")
            return
        if a is None:
            return
        if a["label"] != b["label"]:
            self.problem(where, f"label {a['label']}, expected {b['label']}")
        self.values(where + " values", a["values"], b["values"], VALUE_ATOL, VALUE_RTOL)
        if [s[:3] for s in a["spans"]] != [s[:3] for s in b["spans"]]:
            self.problem(where, f"spans {[s[:3] for s in a['spans']]}, expected {[s[:3] for s in b['spans']]}")
        else:
            self.values(where + " span confidences", [s[3] for s in a["spans"]], [s[3] for s in b["spans"]],
                        VALUE_ATOL, VALUE_RTOL)
        if a["byte_map"] != b["byte_map"]:
            self.problem(where, "byte map differs")

    def dumps(self, a, b):
        if len(a["inputs"]) != len(b["inputs"]):
            self.problem("inputs", f"{len(a['inputs'])} inputs, expected {len(b['inputs'])}")
            return self
        for i, (ia, ib) in enumerate(zip(a["inputs"], b["inputs"])):
            wa, wb = ia["windows"], ib["windows"]
            if [(w["start"], w["length"]) for w in wa] != [(w["start"], w["length"]) for w in wb]:
                self.problem(f"input {i}", "the windows differ")
                continue
            for w, (x, y) in enumerate(zip(wa, wb)):
                where = f"input {i} window {w} (start {x['start']})"
                self.windows += 1
                self.same_digests += x.get("digest") == y.get("digest")
                for key in ("flags", "label"):
                    if x[key] != y[key]:
                        self.problem(where, f"{key} {x[key]}, expected {y[key]}")
                self.values(where + " p_positive", [x["p_positive"]], [y["p_positive"]], VALUE_ATOL, VALUE_RTOL)
                if len(x["heads"]) != len(y["heads"]):
                    self.problem(where, "different head counts")
                    continue
                for h, (ha, hb) in enumerate(zip(x["heads"], y["heads"])):
                    self.head(f"{where} head {h}", ha, hb)
            if "hidden" in ib:
                flat_a = [v for row in ia.get("hidden", []) for v in row]
                flat_b = [v for row in ib["hidden"] for v in row]
                self.values(f"input {i} hidden states", flat_a, flat_b, HIDDEN_ATOL, HIDDEN_RTOL)
        return self


def finish(comparisons, what):
    """Prints a summary and returns the exit status."""
    failed = [c for c in comparisons if c.problems]
    for c in comparisons:
        status = "FAIL" if c.problems else "ok  "
        print(f"{status} {c.label}: {c.windows} windows, {c.same_digests} bit-identical, "
              f"largest value error {c.max_value_error:.2g} of the tolerance")
        for p in c.problems:
            print("     " + p)
    print(f"{what}: {len(comparisons) - len(failed)}/{len(comparisons)} comparisons agree")
    return 1 if failed else 0
