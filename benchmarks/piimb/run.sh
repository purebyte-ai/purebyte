#!/usr/bin/env bash
# Score a PureByte model on the PII Masking Benchmark (PIIMB) with the purebyte CLI, the leaderboard's way.
#
#   benchmarks/piimb/run.sh PATH/TO/PIIMB [MODEL]
#
#   PATH/TO/PIIMB   the folder holding test_sentences.jsonl (CC BY-NC 4.0: evaluation only; see README.md)
#   MODEL           an installed specialist (default: pii) or the path of a .gguf file
#
# Needs python3 (3.8 or newer) and the `purebyte` CLI on PATH. Every sentence of the `sentences` subset is written as a
# file and scanned on its own, as every model of the leaderboard runs; then score.py computes the character-level,
# label-agnostic precision, recall, F1 and F2 per task and their mean over the six tasks.
# Results go to ./piimb-results/MODEL/: scores.md and scores.json (the inputs are kept in inputs/ for another run).
# Nothing here prints a text of the benchmark.
#
# Environment overrides: PUREBYTE (the CLI to run), PYTHON (default python3), THREADS (default: the CLI's), BIAS (an
# operating point instead of the one stored in the file, passed as --bias), EXCLUDE (a JSON file with `uids`: also score
# without those sentences, e.g. the PIIMB_OVERLAP.json of data/build_pii_pool.py).
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
piimb=$(cd "${1:?usage: run.sh PATH/TO/PIIMB [MODEL]}" && pwd)
model=${2:-pii}
purebyte=${PUREBYTE:-purebyte}
python=${PYTHON:-python3}
name=$(basename "$model" .gguf)${BIAS:+-bias${BIAS}}
out=$(pwd)/piimb-results/$name
mkdir -p "$out"

echo "== model"
if [ ! -f "$model" ]; then
  "$purebyte" models pull "$model"
fi
"$purebyte" info "$model" > "$out/model.json"

echo "== inputs"
"$python" "$here/score.py" inputs --piimb "$piimb" --out "$out/.."

echo "== purebyte scan (every sentence on its own)"
args=(scan --model "$model" --format jsonl)
[ -n "${THREADS:-}" ] && args+=(--threads "$THREADS")
[ -n "${BIAS:-}" ] && args+=(--bias "$BIAS")
# Exit status 0 = nothing found, 1 = findings; anything else stops the run.
rc=0
"$purebyte" "${args[@]}" "$out/../inputs" > "$out/purebyte.jsonl" || rc=$?
[ "$rc" -le 1 ] || { echo "purebyte scan failed (exit status $rc)" >&2; exit "$rc"; }

echo "== scores"
score_args=(score --piimb "$piimb" --report "$name=$out/purebyte.jsonl" --json "$out/scores.json")
[ -n "${EXCLUDE:-}" ] && score_args+=(--exclude "$EXCLUDE")
"$python" "$here/score.py" "${score_args[@]}" | tee "$out/scores.md"
