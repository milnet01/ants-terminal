#!/usr/bin/env bash
# Wire this clone's git hooks (ANTS-5542). Safe to re-run.
#
# tools/hooks holds pre-commit and a pre-push shim that hands off to the
# machine-wide hook (~/.claude/githooks/pre-push). The ants.gate.* keys tell
# that hook how to run tools/local-ci.sh here.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

git config core.hooksPath tools/hooks
git config ants.gate.command ./tools/local-ci.sh
git config ants.gate.docsMode --docs
# One list: ci.yml's push paths-ignore decides what is documentation.
git config ants.gate.docsCommand 'python3 tools/ci_workflow.py docs-only'
# Keep the warm build/ tree; refuse a dirty tree rather than cold-build a
# fresh checkout.
git config ants.gate.inPlace true
git config ants.gate.dirtyTree refuse

git config --local --get-regexp '^(core\.hookspath|ants\.gate\.)'
