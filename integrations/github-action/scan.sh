#!/usr/bin/env bash
# Runs `purebyte scan` for the GitHub Action and writes the SARIF report and the number of findings: all of them,
# errors and warnings (findings in test, example and documentation paths), and whether they fail the check.
#
# Environment: PUREBYTE_MODEL, PUREBYTE_PATH, PUREBYTE_SCOPE (changed | all), PUREBYTE_SARIF, PUREBYTE_STRICT (true |
# false), PUREBYTE_ARGS, PUREBYTE_UPLOAD (true | false: the report goes to code scanning), PR_BASE_SHA,
# MERGE_GROUP_BASE_SHA and PUSH_BEFORE_SHA (from the event), plus the standard GitHub Actions variables.
set -euo pipefail

model=${PUREBYTE_MODEL:-secrets-code}
path=${PUREBYTE_PATH:-.}
scope=${PUREBYTE_SCOPE:-changed}
sarif=${PUREBYTE_SARIF:-purebyte.sarif}
extra=()
[ -z "${PUREBYTE_ARGS:-}" ] || read -r -a extra <<< "$PUREBYTE_ARGS"

py() { if command -v python3 > /dev/null 2>&1; then python3 "$@"; else python "$@"; fi; }

# pull_request_target runs with a write token on the base branch: the checkout is the base, so a diff finds nothing
# (a silent pass), and checking out the pull request instead would run untrusted content with that token.
if [ "${GITHUB_EVENT_NAME:-}" = pull_request_target ]; then
  echo "::error title=PureByte::pull_request_target scans the base branch, not the pull request: run this action on pull_request"
  exit 2
fi
# Clear values in a report uploaded to code scanning would be stored in the repository's alerts.
if [ "${PUREBYTE_UPLOAD:-false}" = true ]; then
  for arg in ${extra[@]+"${extra[@]}"}; do
    if [ "$arg" = --reveal ]; then
      echo "::error title=PureByte::--reveal would upload the detected values to code scanning: set upload-sarif: false to use it"
      exit 2
    fi
  done
fi

# What to compare with, when only the changes should be scanned.
base=""
if [ "$scope" = changed ]; then
  case "${GITHUB_EVENT_NAME:-}" in
    pull_request) base=${PR_BASE_SHA:-} ;;
    merge_group) base=${MERGE_GROUP_BASE_SHA:-} ;;  # a merge queue: the changes of the group over its base
    push) base=${PUSH_BEFORE_SHA:-} ;;
  esac
  case "$base" in 0000000000000000000000000000000000000000) base="" ;; esac
  if [ -n "$base" ] && ! git -C "$path" cat-file -e "${base}^{commit}" 2> /dev/null; then
    # Shallow checkouts do not have the base commit: fetch just that commit.
    git -C "$path" fetch --no-tags --depth=1 origin "$base" > /dev/null 2>&1 || base=""
  fi
  [ -z "$base" ] && echo "No base commit to compare with: scanning everything under ${path}."
fi

cmd=(purebyte scan --model "$model" --format sarif --out "$sarif")
[ "${PUREBYTE_STRICT:-false}" = true ] && cmd+=(--strict)
if [ -n "$base" ]; then
  cmd+=(--git-diff "$base")
  echo "Scanning the changes since ${base:0:12} with ${model}"
else
  echo "Scanning ${path} with ${model}"
fi
cmd+=(${extra[@]+"${extra[@]}"} "$path")

rm -f "$sarif"
set +e
"${cmd[@]}"
status=$?
set -e

# Exit status: 0 = no finding of severity error (warnings alone do not count unless --strict), 1 = findings that fail
# the check, 2 = something could not be scanned (the report lists what and why) or the command failed (then there is
# no report).
if [ "$status" -gt 1 ] && [ ! -s "$sarif" ]; then
  echo "::error title=PureByte::the scan failed with exit status ${status}"
  exit "$status"
fi
failing=false
[ "$status" -eq 1 ] && failing=true

read -r findings errors warnings not_scanned < <(py -c '
import json, sys
runs = json.load(open(sys.argv[1], encoding="utf-8")).get("runs", [])
results = [x for r in runs for x in r.get("results", [])]
warnings = sum(x.get("level") == "warning" for x in results)
print(len(results), len(results) - warnings, warnings,
      sum(len(i.get("toolExecutionNotifications", [])) for r in runs for i in r.get("invocations", [])))' "$sarif" |
  tr -d '\r')  # Windows runners: Python ends the line with CR LF
echo "PureByte: ${findings} finding(s) (${errors} error(s), ${warnings} warning(s)), ${not_scanned} not scanned"
# One annotation per input that was not scanned, with the reason the CLI gives (never any content).
py -c '
import json, sys
for r in json.load(open(sys.argv[1], encoding="utf-8")).get("runs", []):
    for i in r.get("invocations", []):
        for n in i.get("toolExecutionNotifications", []):
            where = [l.get("physicalLocation", {}).get("artifactLocation", {}).get("uri", "") for l in n.get("locations", [])]
            print("::warning title=PureByte (not scanned)::%s: %s" % (where[0] if where else "?", n.get("message", {}).get("text", "")))' "$sarif"
{
  echo "findings=${findings}"
  echo "errors=${errors}"
  echo "warnings=${warnings}"
  echo "failing=${failing}"
  echo "sarif-file=${sarif}"
} >> "${GITHUB_OUTPUT:-/dev/null}"

if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
  {
    echo "### PureByte (${model})"
    echo
    echo "${errors} possible credential(s). Values are masked; details are in code scanning and in the log."
    if [ "$warnings" != 0 ] && [ "${PUREBYTE_STRICT:-false}" = true ]; then
      echo "${warnings} warning(s) in test, example or documentation paths, which fail the check too (strict)."
    elif [ "$warnings" != 0 ]; then
      echo "${warnings} warning(s) in test, example or documentation paths: reported, without failing the check."
    fi
    [ "$not_scanned" = 0 ] || echo "${not_scanned} input(s) could not be scanned: see the log of the scan step."
  } >> "$GITHUB_STEP_SUMMARY"
fi

# Findings are uploaded and reported first; then an incomplete scan fails the step, whatever `fail-on-findings` says.
if [ "$status" -gt 1 ]; then
  echo "::error title=PureByte::${not_scanned} input(s) could not be scanned (for example, files over the model's size limit); see the log above"
  exit "$status"
fi
