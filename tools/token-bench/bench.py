#!/usr/bin/env python3
"""SIGN-1 token bench runner (ANTS-5545; measured by ANTS-1120).

Gives Claude Code the fixed prompt in README.md against a fresh copy of
project/, once per run, and records what the run cost.

Usage:
    tools/token-bench/bench.py run --arm ants|plain [--runs N] [--out DIR]
    tools/token-bench/bench.py summary [--out DIR]

The two arms differ only in Ants:
    ants   the user's Claude Code settings and MCP servers as they are, with
           the environment of an Ants tab.
    plain  the same, minus the "ants" MCP server, minus every hook whose
           command matches ANTS_HOOK_MARKERS, and minus the ANTS_* and
           TERM_PROGRAM* variables: what Claude Code sees in Konsole once Ants
           is taken out.

Each run is headless (`claude -p`), so it is not attached to an Ants tab and
the model auto-switcher, which acts on a tab, cannot act on it.

A run's counts come from its session transcript under ~/.claude/projects/:
every assistant message's API `usage` field, one per message id (a message
split over several transcript lines repeats its usage), including subagent
transcripts. Cost is those counts at the published price in PRICES.

After the run the pristine tests/ are copied back over the copy before the
bug's test runs, so a run that edited the test cannot pass.

Each run appends one JSON line to DIR/results.jsonl. `summary` reads it.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import time
import uuid
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROJECT = HERE / "project"
README = HERE / "README.md"
DEFAULT_OUT = Path("/mnt/Emulators/token-bench-runs")
BUG_TEST = "tests.test_trip_example"

# USD per million tokens. Source: platform.claude.com/docs/en/about-claude/pricing,
# read 2026-10-15. Global routing (the default), so no regional multiplier.
PRICES = {
    "claude-opus-5-5": {
        "input": 4.00, "cache_write_5m": 5.00, "cache_write_1h": 8.00,
        "cache_read": 0.20, "output": 20.00,
    },
}
TYPES = ["input", "cache_write_5m", "cache_write_1h", "cache_read", "output"]

# A hook is an Ants hook when its command contains one of these.
ANTS_HOOK_MARKERS = ("/ants-terminal/", "/Ants Terminal/", "/hooks/ants-", "roadmap-store-guard")
ANTS_MCP_SERVER = "ants"


def bench_config() -> tuple[str, str]:
    """The fixed prompt and model, read from README.md so it is their one home."""
    text = README.read_text(encoding="utf-8")
    model = re.search(r"^\*\*Model:\*\* `([^`]+)`", text, re.M)
    prompt = re.search(r"<!-- bench-prompt -->\n```text\n(.*?)\n```", text, re.S)
    if not model or not prompt:
        sys.exit("bench: README.md has lost its **Model:** line or its bench-prompt block")
    return prompt.group(1), model.group(1)


def is_ants_hook(hook: dict) -> bool:
    return any(m in hook.get("command", "") for m in ANTS_HOOK_MARKERS)


def arm_settings(arm: str) -> tuple[dict, list[str]]:
    """The user's settings for this arm, and the hook commands it removed."""
    settings = json.loads((Path.home() / ".claude/settings.json").read_text())
    removed = []
    if arm == "plain":
        hooks = {}
        for event, groups in settings.get("hooks", {}).items():
            kept_groups = []
            for group in groups:
                kept = [h for h in group.get("hooks", []) if not is_ants_hook(h)]
                removed += [f"{event}: {h['command']}" for h in group.get("hooks", []) if is_ants_hook(h)]
                if kept:
                    kept_groups.append({**group, "hooks": kept})
            if kept_groups:
                hooks[event] = kept_groups
        settings["hooks"] = hooks
    return settings, removed


def arm_mcp(arm: str) -> dict:
    servers = json.loads((Path.home() / ".claude.json").read_text()).get("mcpServers", {})
    if arm == "plain":
        servers = {k: v for k, v in servers.items() if k != ANTS_MCP_SERVER}
    return {"mcpServers": servers}


def arm_env(arm: str) -> dict:
    # This session's own Claude Code variables would leak into the child in
    # both arms (CLAUDE_EFFORT among them), so neither arm gets them.
    env = {k: v for k, v in os.environ.items()
           if not k.startswith("CLAUDE_") and k != "CLAUDECODE"}
    if arm == "plain":
        env = {k: v for k, v in env.items()
               if not k.startswith("ANTS_") and not k.startswith("TERM_PROGRAM")}
    return env


def transcript_files(session_id: str) -> list[Path]:
    root = Path.home() / ".claude/projects"
    main = list(root.glob(f"*/{session_id}.jsonl"))
    subagents = list(root.glob(f"*/{session_id}/subagents/*.jsonl"))
    return main + subagents


def usage_from_transcripts(files: list[Path]) -> dict:
    """Token counts by type and model, one per API message id."""
    by_message = {}
    for path in files:
        for line in path.read_text(encoding="utf-8").splitlines():
            try:
                entry = json.loads(line)
            except json.JSONDecodeError:
                continue
            msg = entry.get("message") if entry.get("type") == "assistant" else None
            if not isinstance(msg, dict) or not msg.get("usage") or not msg.get("id"):
                continue
            by_message[msg["id"]] = (msg.get("model", "?"), msg["usage"])
    totals = {}
    for model, u in by_message.values():
        t = totals.setdefault(model, dict.fromkeys(TYPES, 0) | {"messages": 0})
        cc = u.get("cache_creation") or {}
        write_1h = cc.get("ephemeral_1h_input_tokens", 0)
        write_5m = cc.get("ephemeral_5m_input_tokens",
                          u.get("cache_creation_input_tokens", 0) - write_1h)
        t["input"] += u.get("input_tokens", 0)
        t["cache_write_5m"] += write_5m
        t["cache_write_1h"] += write_1h
        t["cache_read"] += u.get("cache_read_input_tokens", 0)
        t["output"] += u.get("output_tokens", 0)
        t["messages"] += 1
    return totals


def cost_usd(totals: dict) -> float | None:
    """None when a model in the run has no price in PRICES."""
    cost = 0.0
    for model, t in totals.items():
        price = PRICES.get(model)
        if price is None:
            return None
        cost += sum(t[k] * price[k] for k in TYPES) / 1e6
    return round(cost, 4)


def bug_test_passes(workdir: Path) -> bool:
    shutil.rmtree(workdir / "tests")
    shutil.copytree(PROJECT / "tests", workdir / "tests")
    result = subprocess.run([sys.executable, "-m", "unittest", BUG_TEST],
                            cwd=workdir, capture_output=True, text=True)
    return result.returncode == 0


def run_once(arm: str, out: Path, index: int, timeout: int, budget: float) -> dict:
    prompt, model = bench_config()
    stamp = time.strftime("%Y%m%d-%H%M%S")
    workdir = out / f"{arm}-{stamp}-{index}"
    shutil.copytree(PROJECT, workdir, ignore=shutil.ignore_patterns("__pycache__"))
    subprocess.run(["git", "init", "-q"], cwd=workdir, check=True)
    subprocess.run(["git", "add", "-A"], cwd=workdir, check=True)
    subprocess.run(["git", "-c", "user.name=bench", "-c", "user.email=bench@localhost",
                    "commit", "-qm", "bench start"], cwd=workdir, check=True)

    settings, removed = arm_settings(arm)
    settings_file = out / f"{workdir.name}.settings.json"
    mcp_file = out / f"{workdir.name}.mcp.json"
    settings_file.write_text(json.dumps(settings, indent=1))
    mcp = arm_mcp(arm)
    mcp_file.write_text(json.dumps(mcp, indent=1))

    session_id = str(uuid.uuid4())
    cmd = ["claude", "-p", prompt, "--model", model, "--effort", "high",
           "--session-id", session_id, "--output-format", "json",
           "--permission-mode", "bypassPermissions",
           "--setting-sources", "project,local", "--settings", str(settings_file),
           "--strict-mcp-config", "--mcp-config", str(mcp_file),
           "--max-budget-usd", str(budget)]
    started = time.time()
    try:
        proc = subprocess.run(cmd, cwd=workdir, env=arm_env(arm), capture_output=True,
                              text=True, timeout=timeout)
        exit_code, stdout = proc.returncode, proc.stdout
    except subprocess.TimeoutExpired as err:
        exit_code, stdout = "timeout", err.stdout or ""
        stdout = stdout.decode() if isinstance(stdout, bytes) else stdout
    seconds = round(time.time() - started)

    try:
        cli_result = json.loads(stdout)
    except json.JSONDecodeError:
        cli_result = {}
    totals = usage_from_transcripts(transcript_files(session_id))
    record = {
        "arm": arm, "model": model, "session_id": session_id, "workdir": str(workdir),
        "started": stamp, "seconds": seconds, "exit": exit_code,
        "bug_test_passed": bug_test_passes(workdir),
        "tokens": totals, "cost_usd": cost_usd(totals),
        "cli_total_cost_usd": cli_result.get("total_cost_usd"),
        "cli_num_turns": cli_result.get("num_turns"),
        "mcp_servers": sorted(mcp["mcpServers"]), "hooks_removed": removed,
    }
    with open(out / "results.jsonl", "a", encoding="utf-8") as fh:
        fh.write(json.dumps(record) + "\n")
    return record


def summary(out: Path) -> None:
    rows = [json.loads(l) for l in (out / "results.jsonl").read_text().splitlines() if l.strip()]
    means = {}
    for arm in ("ants", "plain"):
        runs = [r for r in rows if r["arm"] == arm]
        counted = [r for r in runs if r["bug_test_passed"] and r["cost_usd"] is not None]
        print(f"\n{arm}: {len(runs)} runs, {len(counted)} counted (bug test passed)")
        for r in runs:
            t = {k: sum(m[k] for m in r["tokens"].values()) for k in TYPES}
            print(f"  {r['started']}  passed={r['bug_test_passed']!s:5}  ${r['cost_usd']}"
                  f"  (cli ${r['cli_total_cost_usd']})  {r['seconds']}s  "
                  + "  ".join(f"{k}={t[k]}" for k in TYPES))
        costs = [r["cost_usd"] for r in counted]
        if costs:
            sd = statistics.stdev(costs) if len(costs) > 1 else 0.0
            means[arm] = (statistics.mean(costs), sd)
            print(f"  mean ${means[arm][0]:.4f}  sd ${sd:.4f}")
    if len(means) == 2:
        (a, sa), (p, sp) = means["ants"], means["plain"]
        saving = 100 * (p - a) / p
        print(f"\nAnts costs {abs(saving):.1f}% {'less' if saving >= 0 else 'more'} than plain"
              " (SIGN-1 asks for at least 20% less)")
        print(f"plain/ants ratio {p / a:.3f}; conservative (plain-1sd)/(ants+1sd) "
              f"{(p - sp) / (a + sa):.3f}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    run = sub.add_parser("run")
    run.add_argument("--arm", choices=["ants", "plain"], required=True)
    run.add_argument("--runs", type=int, default=1)
    run.add_argument("--out", type=Path, default=DEFAULT_OUT)
    run.add_argument("--timeout", type=int, default=1800, help="seconds per run")
    run.add_argument("--budget", type=float, default=10.0, help="USD cap per run")
    summ = sub.add_parser("summary")
    summ.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = parser.parse_args()
    if args.cmd == "summary":
        summary(args.out)
        return 0
    args.out.mkdir(parents=True, exist_ok=True)
    for i in range(1, args.runs + 1):
        record = run_once(args.arm, args.out, i, args.timeout, args.budget)
        print(json.dumps({k: record[k] for k in
                          ("arm", "started", "exit", "bug_test_passed", "cost_usd",
                           "cli_total_cost_usd", "seconds")}), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
