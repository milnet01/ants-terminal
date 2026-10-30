#!/usr/bin/env bash
# ants-terminal SessionStart hook — ANTS-6310.
# Says that the machine-wide know-how store exists, in one line, and prints
# nothing until some session has written to it. It only tests for the file, so
# it adds no process to a session's start. Unlike most of the pack it is not
# gated on `.ants-project`, because the store serves every project.

store="${XDG_DATA_HOME:-$HOME/.local/share}/ants-terminal/knowhow.sqlite"
if [ -e "$store" ]; then
    printf '%s\n' '[ants:know-how] shared how-to facts for machines and tools: know_how op:"find"'
fi
exit 0
