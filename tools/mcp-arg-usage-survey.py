#!/usr/bin/env python3
"""Measure how often each argument of an Ants MCP verb is passed.

ANTS-5725 keeps visible every argument used in at least 5% of a verb's calls
(user decision, 2026-10-16) and lists the rest only through tool_info. This
reads Claude Code's session transcripts and counts, per verb, the calls that
passed each argument.

Usage:
    tools/mcp-arg-usage-survey.py [--since YYYY-MM-DD] VERB [VERB ...]
    tools/mcp-arg-usage-survey.py workspace_search file_outline read_region find_definition

Transcripts are read from ~/.claude/projects/*/*.jsonl (subagent transcripts
included). A line that is not JSON is skipped, not guessed at.
"""
import collections
import glob
import json
import os
import sys


def main():
    args = sys.argv[1:]
    since = None
    if len(args) >= 2 and args[0] == "--since":
        since, args = args[1], args[2:]
    if not args:
        sys.exit(__doc__)
    wanted = {"mcp__ants__" + v: v for v in args}
    calls = collections.Counter()
    uses = collections.defaultdict(collections.Counter)
    pattern = os.path.expanduser("~/.claude/projects/**/*.jsonl")
    for path in glob.iglob(pattern, recursive=True):
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                if "mcp__ants__" not in line:
                    continue
                try:
                    rec = json.loads(line)
                except ValueError:
                    continue
                if since and rec.get("timestamp", "")[:10] < since:
                    continue
                content = (rec.get("message") or {}).get("content")
                if not isinstance(content, list):
                    continue
                for block in content:
                    if not isinstance(block, dict) or block.get("type") != "tool_use":
                        continue
                    verb = wanted.get(block.get("name"))
                    if not verb or not isinstance(block.get("input"), dict):
                        continue
                    calls[verb] += 1
                    for key in block["input"]:
                        uses[verb][key] += 1
    for verb in args:
        n = calls[verb]
        print(f"{verb}: {n} calls")
        for key, k in uses[verb].most_common():
            print(f"  {key:24} {k:7}  {k / n:6.1%}")


if __name__ == "__main__":
    main()
