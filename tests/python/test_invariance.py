"""Bit-exactness of the engine with itself: every kernel (scalar, and the SIMD kernel of this CPU) and every thread
arrangement (one thread, a batch split over teams, windows split within a team, a pool that does not divide into
teams) must give the SAME BITS for every random model of tests/gen (AGENTS.md, "Exactness and determinism").

    python tests/python/test_invariance.py --model-dump build/tests/model_dump --work build/tests/work
"""
import os
import sys

import common

ARRANGEMENTS = {
    "4 threads, batch": ["--threads=4"],
    "4 threads, one team": ["--threads=4", "--intra-threads=4"],
    "3 threads, teams of 2": ["--threads=3", "--intra-threads=2"],
    "5 threads, teams of 3": ["--threads=5", "--intra-threads=3"],
    "scalar kernel": ["--kernel=scalar"],
    "scalar kernel, 2 threads per window": ["--kernel=scalar", "--threads=2", "--intra-threads=2"],
}


def strip(dump):
    return {k: v for k, v in dump.items() if k != "kernel"}


def main():
    args = common.arguments(__doc__)
    models = common.generate_models(args.work)
    failures = 0
    for name in args.model or list(common.random_models.MODELS):
        path, inputs = os.path.join(models, name + ".gguf"), os.path.join(models, name + ".inputs.bin")
        for variant, options in common.variants_for(name).items():
            if variant not in ("default", "early-exit", "chunked", "whole"):
                continue
            # The digests cover every window's hidden states; --hidden adds pb_encode (one sequence, all threads).
            hidden = ["--hidden"] if variant == "default" else []
            base = common.model_dump(args.model_dump, path, inputs, options + hidden)
            for label, extra in ARRANGEMENTS.items():
                other = common.model_dump(args.model_dump, path, inputs, options + extra + hidden)
                same = strip(other) == strip(base)
                failures += not same
                print(f"{'ok  ' if same else 'FAIL'} {name} [{variant}] {label} ({other['kernel']}): "
                      f"{'identical' if same else 'DIFFERENT'} to 1 thread ({base['kernel']})")
    print(f"invariance: {'all identical' if not failures else f'{failures} differences'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
