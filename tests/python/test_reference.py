"""The engine against the NumPy reference (reference/), on every random model of tests/gen and every scan variant:
decisions (windows, flags, labels, spans, byte maps) must be identical and values within the tolerance of
tests/README.md. The final hidden states are compared too, on the default variant.

    python tests/python/test_reference.py --model-dump build/tests/model_dump --work build/tests/work
"""
import os
import sys

import common


def main():
    args = common.arguments(__doc__)
    try:
        import numpy  # noqa: F401
    except ImportError:
        print("NumPy is not installed: the reference comparison is skipped")
        return common.SKIP
    models = common.generate_models(args.work)
    comparisons = []
    for name in args.model or list(common.random_models.MODELS):
        path, inputs = os.path.join(models, name + ".gguf"), os.path.join(models, name + ".inputs.bin")
        for variant, options in common.variants_for(name).items():
            if variant == "default":
                options = options + ["--hidden"]
            engine = common.model_dump(args.model_dump, path, inputs, options)
            reference = common.reference_dump(path, inputs, options)
            comparisons.append(common.Comparison(f"{name} [{variant}]").dumps(engine, reference))
    return common.finish(comparisons, "engine vs reference")


if __name__ == "__main__":
    sys.exit(main())
