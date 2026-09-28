"""The engine against the golden files of tests/golden: what the engine computed for every random model of tests/gen
when the goldens were made. Decisions must be identical everywhere; values must agree within the tolerance of
tests/README.md; on the platform that made the goldens every window must also be bit for bit identical (digests).

    python tests/python/test_golden.py --model-dump build/tests/model_dump --work build/tests/work [--update]

--update rewrites the goldens. Do it only when a change is MEANT to alter results, and say which and why in the pull
request (AGENTS.md, "Golden files are never regenerated to make a test pass").
"""
import hashlib
import json
import os
import sys

import common

GOLDEN = os.path.join(common.ROOT, "tests", "golden")
# The variants locked by goldens (the reference test runs all of them).
LOCKED = ("default", "early-exit", "type-bias", "chunked", "resized")


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def main():
    update = "--update" in sys.argv
    if update:
        sys.argv.remove("--update")
    args = common.arguments(__doc__)
    models = common.generate_models(args.work)
    comparisons, written = [], 0
    for name in args.model or list(common.random_models.MODELS):
        path, inputs = os.path.join(models, name + ".gguf"), os.path.join(models, name + ".inputs.bin")
        variants = {k: v for k, v in common.variants_for(name).items() if k in LOCKED}
        dumps = {k: common.model_dump(args.model_dump, path, inputs, v) for k, v in variants.items()}
        golden_path = os.path.join(GOLDEN, name + ".json")
        if update:
            golden = {"model": name, "model_sha256": sha256(path), "inputs_sha256": sha256(inputs),
                      "platform": next(iter(dumps.values()))["platform"],
                      "variants": {k: {"options": variants[k], "dump": d} for k, d in dumps.items()}}
            os.makedirs(GOLDEN, exist_ok=True)
            with open(golden_path, "w", encoding="utf-8", newline="\n") as f:
                json.dump(golden, f, indent=None, separators=(",", ":"))
                f.write("\n")
            written += 1
            continue
        with open(golden_path, encoding="utf-8") as f:
            golden = json.load(f)
        stale = golden["model_sha256"] != sha256(path) or golden["inputs_sha256"] != sha256(inputs)
        for variant, dump in dumps.items():
            c = common.Comparison(f"{name} [{variant}]")
            comparisons.append(c)
            if stale:
                c.problem("golden", "tests/gen produced another model or other inputs than the golden was made from")
            if variant not in golden["variants"]:
                c.problem("golden", "no golden for this variant")
                continue
            c.dumps(dump, golden["variants"][variant]["dump"])
            if dump["platform"] == golden["platform"] and not c.problems and c.same_digests != c.windows:
                c.problem("digests", f"{c.windows - c.same_digests} of {c.windows} windows are not bit-identical to "
                                     f"the golden made on this platform ({golden['platform']})")
    if update:
        print(f"wrote {written} golden files to {GOLDEN}")
        return 0
    return common.finish(comparisons, "engine vs golden files")


if __name__ == "__main__":
    sys.exit(main())
