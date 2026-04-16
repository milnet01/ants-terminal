#!/usr/bin/env bash
#
# hook-on-cmakelists-edit.sh — PostToolUse hook for the Ants Terminal
# project. Runs check-version-drift.sh whenever Edit/Write touched a
# CMakeLists.txt anywhere under the project, and surfaces drift via the
# hook's systemMessage so the model sees it immediately rather than
# discovering it at commit time.
#
# Wired in via .claude/settings.json:
#     PostToolUse → matcher Edit|Write → command bash <this script>
#
# Stdin: PostToolUse JSON payload from Claude Code. We extract the
# touched file path and short-circuit unless it's a CMakeLists.txt.
# Stdout: a JSON object containing systemMessage, OR nothing (silent
# pass).
#
# Exit code: always 0. We never want to block or fail an Edit; just
# surface drift information.
#
# Why a script not an inline command: keeps the JSON in settings.json
# small + readable + diffable, and lets us iterate on the script
# without re-validating settings.json schema.

set -u

PROJECT_ROOT="/mnt/Storage/Scripts/Linux/Ants"

file=$(jq -r '.tool_input.file_path // .tool_response.filePath // empty' 2>/dev/null)
case "$file" in
    */CMakeLists.txt|CMakeLists.txt) ;;
    *) exit 0 ;;
esac

drift=$(cd "$PROJECT_ROOT" && bash packaging/check-version-drift.sh 2>&1)
if [ -n "$drift" ]; then
    jq -n --arg d "$drift" \
        '{systemMessage: ("⚠ packaging-version-drift after CMakeLists.txt edit:\n" + $d)}'
fi
exit 0
