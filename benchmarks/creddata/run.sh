#!/usr/bin/env bash
# Reproduce the CredData results in benchmarks/RESULTS.md with the released CLI and model.
#
#   benchmarks/creddata/run.sh PATH/TO/CredData [PARTITION] [--whole-files]
#
#   PARTITION      test (default), dev, dev-no-openssl, all or all-no-openssl
#   --whole-files  scan every labelled file entirely instead of the exact scan regions (same labelled results,
#                  about 14 times more bytes to scan on TEST)
#
# Needs python3 (3.8 or newer) and the `purebyte` CLI on PATH. gitleaks and trufflehog are run and scored too when
# they are on PATH (trufflehog always with --no-verification: nothing is ever sent to any service).
# Results go to ./creddata-results/PARTITION/: scores.md and scores.json for every repository of the partition, and
# scores-excluded.md and scores-excluded.json without the repositories in EXCLUDE_REPOS (by default the four TEST
# repositories that overlap the training corpus of the first released models: the published TEST figures). Nothing
# here prints a credential value.
#
# Environment overrides: PUREBYTE (the CLI to run), MODEL (default secrets-code), BATCH (inputs per CLI call),
# EXCLUDE_REPOS (default training-overlap; set it empty to skip the second table).
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
creddata=$(cd "${1:?usage: run.sh PATH/TO/CredData [PARTITION] [--whole-files]}" && pwd)
partition=${2:-test}
mode=${3:-regions}
purebyte=${PUREBYTE:-purebyte}
model=${MODEL:-secrets-code}
batch=${BATCH:-500}
exclude=${EXCLUDE_REPOS-training-overlap}
out=$(pwd)/creddata-results/$partition
mkdir -p "$out"

score() { python3 "$here/score.py" "$@" --creddata "$creddata"; }

echo "== CredData split"
score split --partitions "$partition"

echo "== model"
"$purebyte" models pull "$model"

echo "== inputs"
region_args=()
if [ "$mode" = "--whole-files" ]; then
  score files --partition "$partition" | sed "s|^|$creddata/|" > "$out/inputs.txt"
  echo "$(wc -l < "$out/inputs.txt") whole files"
else
  rm -rf "$out/regions"
  score regions --partition "$partition" --out "$out/regions"
  cp "$out/regions/files.txt" "$out/inputs.txt"
  region_args=(--regions "$out/regions")
fi

echo "== purebyte scan (this is the long step)"
# Inputs are passed explicitly: a directory walk would skip file types the scanner does not select by default.
# Exit status 0 = no finding of severity error, 1 = some; anything else stops the run. Every finding is scored,
# warnings (test, example and documentation paths) included.
: > "$out/purebyte.json"
tr -d '\r' < "$out/inputs.txt" | tr '\n' '\0' |
  xargs -0 -n "$batch" sh -c 'cli=$0 model=$1 dest=$2; shift 2
    "$cli" scan --model "$model" --format json "$@" >> "$dest"; rc=$?; [ "$rc" -le 1 ] || exit 255' \
    "$purebyte" "$model" "$out/purebyte.json"

reports=(--purebyte "$model=$out/purebyte.json")

if command -v gitleaks > /dev/null 2>&1; then
  echo "== gitleaks $(gitleaks version)"
  gitleaks detect --no-git --source "$creddata/data" --report-format json --report-path "$out/gitleaks.json" \
    --redact --no-banner --exit-code 0 --log-level error
  reports+=(--gitleaks "gitleaks=$out/gitleaks.json")
fi

if command -v trufflehog > /dev/null 2>&1; then
  echo "== $(trufflehog --version 2>&1 | head -1)"
  trufflehog filesystem "$creddata/data" --json --no-verification --no-update --log-level=-1 > "$out/trufflehog.jsonl" || true
  reports+=(--trufflehog "trufflehog=$out/trufflehog.jsonl")
fi

echo "== scores: every repository of the partition"
score score "${reports[@]}" "${region_args[@]}" --partitions "$partition" --by-category --json "$out/scores.json" \
  | tee "$out/scores.md"

if [ -n "$exclude" ]; then
  echo "== scores without ${exclude}"
  score score "${reports[@]}" "${region_args[@]}" --partitions "$partition" --by-category --exclude-repo "$exclude" \
    --json "$out/scores-excluded.json" | tee "$out/scores-excluded.md"
fi
