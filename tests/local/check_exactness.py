"""Bit-exactness of this library against a reference engine, on models and data that are NOT part of this repository.

LOCAL tool, never run in CI: it needs real released models and the reference engine's outputs, which live outside the
repository. Everything is described by a JSON file whose path is given in PUREBYTE_EXACTNESS_CONFIG:

    {
      "tool": "build/tests/window_report",           # optional; this library's window_report
      "threads": 6,                                    # optional; default 4
      "cases": [
        {"name": "binary-probe",
         "model": "/models/secrets-bin.gguf",
         "inputs": ["/data/probe.bin"],               # files, scanned as one list (like the reference's @list mode)
         "window": 512, "stride": 512,
         "flags": ["--digest"],                        # optional: --digest, --prefilter, --early-exit
         "reference": "/runs/probe.jsonl",             # the reference engine's output for the same list...
         "reference_command": ["reference_engine", "{model}", "@{list}", "--window=512", "--stride=512"]   # ...or a command
        }
      ]
    }

Both engines print one JSON line per window (`window_report` mirrors the reference's window mode). The check is line by
line: the same windows, labels, spans and, with --digest, the same FNV-1a of the hidden states. Exit status 0 only when
every case is identical. Processes run at the lowest CPU priority.

    PUREBYTE_EXACTNESS_CONFIG=exactness.json python tests/local/check_exactness.py [case-name ...]
"""
import json
import os
import subprocess
import sys
import tempfile
import time

IDLE = 0x00000040  # Windows IDLE_PRIORITY_CLASS


def run(command):
    kwargs = {"capture_output": True}
    if os.name == "nt":
        kwargs["creationflags"] = IDLE
    else:
        kwargs["preexec_fn"] = lambda: os.nice(19)
    done = subprocess.run(command, **kwargs)
    if done.returncode != 0:
        raise SystemExit(f"command failed ({done.returncode}): {' '.join(command)}\n{done.stderr.decode(errors='replace')[-2000:]}")
    return done.stdout.decode("utf-8", "replace").replace("\r\n", "\n")


def default_tool():
    here = os.path.dirname(os.path.abspath(__file__))
    name = "window_report.exe" if os.name == "nt" else "window_report"
    return os.path.join(here, "..", "..", "build", "tests", name)


def lines_of(text):
    return [line for line in text.split("\n") if line.startswith("{")]


def compare(ours, reference):
    """(identical lines, first differences) of two window reports."""
    if len(ours) != len(reference):
        return 0, [f"{len(ours)} windows here, {len(reference)} in the reference"]
    same, differences = 0, []
    for a, b in zip(ours, reference):
        if a == b:
            same += 1
            continue
        if len(differences) < 5:
            ja, jb = json.loads(a), json.loads(b)
            fields = sorted(k for k in set(ja) | set(jb) if ja.get(k) != jb.get(k))
            differences.append(f"window f={jb.get('f')} start={jb.get('start')}: " +
                               ", ".join(f"{k}: {ja.get(k)!r} != {jb.get(k)!r}" for k in fields))
    return same, differences


def main():
    path = os.environ.get("PUREBYTE_EXACTNESS_CONFIG")
    if not path:
        raise SystemExit(__doc__)
    config = json.load(open(path, encoding="utf-8"))
    tool = config.get("tool") or default_tool()
    threads = int(config.get("threads", 4))
    selected = set(sys.argv[1:])
    failures = 0
    for case in config["cases"]:
        if selected and case["name"] not in selected:
            continue
        with tempfile.TemporaryDirectory() as tmp:
            listing = os.path.join(tmp, "inputs.list")
            with open(listing, "w", encoding="utf-8") as f:
                f.write("\n".join(case["inputs"]) + "\n")
            args = [tool, case["model"], "@" + listing, f"--window={case['window']}", f"--stride={case['stride']}",
                    f"--threads={threads}", *case.get("flags", [])]
            started = time.perf_counter()
            ours = lines_of(run(args))
            seconds = time.perf_counter() - started
            if "reference" in case:
                reference = lines_of(open(case["reference"], encoding="utf-8").read().replace("\r\n", "\n"))
            else:
                command = [part.replace("{model}", case["model"]).replace("{list}", listing) for part in case["reference_command"]]
                reference = lines_of(run(command))
        same, differences = compare(ours, reference)
        ok = same == len(reference) and not differences
        failures += not ok
        print(f"{'OK  ' if ok else 'FAIL'} {case['name']}: {same}/{len(reference)} identical window lines ({seconds:.0f} s)", flush=True)
        for d in differences:
            print("     " + d, flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
