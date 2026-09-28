#!/usr/bin/env bash
# Wire this clone's git hooks (ANTS-5542). Safe to re-run.
#
# tools/hooks holds pre-commit and a pre-push shim that hands off to the
# machine-wide hook (~/.claude/githooks/pre-push). How that hook runs the gate
# is committed in .ants/gate.conf; only core.hooksPath has to be set per clone.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

git config core.hooksPath tools/hooks
# A clone's own ants.gate.* keys override .ants/gate.conf. Clear any left from
# before the file existed, so the committed settings govern.
git config --local --remove-section ants.gate 2>/dev/null || true

git config --local --get-regexp '^(core\.hookspath|ants\.gate\.)'
