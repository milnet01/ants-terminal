#!/usr/bin/env bash
# measure-companion-tokens.sh — ANTS-1120 instrumentation harness.
#
# Drives one Claude Code task through two configurations N times each
# and aggregates per-prompt token counts (input + output + cache_read +
# cache_creation, all extracted from the API `usage` field on each
# response). The output journal lands at:
#   docs/journal/ANTS-1120-measurement.md
#
# Configurations:
#   A (baseline)     — Claude reads ROADMAP/runs audit/etc. directly,
#                      summarising stdout in-context as it goes.
#   B (instrumented) — same task shape, but tool calls return stubs
#                      simulating ANTS-1116 / ANTS-1117 outputs.
#
# Stubs are pre-recorded in scripts/measure-companion-stubs/ before the
# run begins (one capture from production tooling per surface). The
# stubs are not invented — they are the actual JSON shapes the helper
# binaries produce against this tree at the time of capture.
#
# Per docs/specs/ANTS-1120.md, threshold values for the
# keep/iterate/drop/inconclusive verdict are decided by the user *after*
# reading the journal — not pre-set in this script. The script's job is
# to produce the data; the verdict is a separate step.
#
# Usage:
#   scripts/measure-companion-tokens.sh [N]      # default N=3
#
# Pre-requisites:
#   - `claude` CLI in PATH with a working API key.
#   - jq for parsing the API logs.
#   - This repo at HEAD with the ANTS-1116 v1 + ANTS-1117 v1 surfaces
#     in tree (so stub captures match what production would emit).

set -euo pipefail

N="${1:-3}"
if ! [[ "$N" =~ ^[0-9]+$ ]] || [[ "$N" -lt 3 ]]; then
    echo "ANTS-1120 INV-2 requires N >= 3 replays per configuration." >&2
    echo "Refusing to run with N=$N." >&2
    exit 2
fi

ROOT="$(git rev-parse --show-toplevel)"
JOURNAL="$ROOT/docs/journal/ANTS-1120-measurement.md"

echo "[ANTS-1120] N=$N replays per configuration"
echo "[ANTS-1120] journal output → $JOURNAL"

cat <<'EOF' >&2
[ANTS-1120] NOTE: this script is a placeholder skeleton landed alongside
ANTS-1120 v1. The actual measurement run requires:
  1. A representative scripted Claude task (TBD when the user picks
     the workload).
  2. Stub captures in scripts/measure-companion-stubs/ — one per
     instrumented surface (audit-run / roadmap-query / id-allocate).
  3. API-log parsing logic to extract per-prompt usage fields.
The skeleton enforces the N>=3 floor so a future pass through this
file can't accidentally short-circuit the variance check.
EOF

# When the run actually executes, this script will:
#   - Fork two child sequences A and B with the same scripted prompts.
#   - Capture each round-trip's API request/response pair to a JSONL log.
#   - Aggregate per-prompt + composite mean + stdev across the N runs.
#   - Write the journal artifact via a Python/jq pipeline.

exit 0
