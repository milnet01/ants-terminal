#!/usr/bin/env python3
"""Run a job of .github/workflows/ci.yml on this machine, from ci.yml itself.

ANTS-5322. Replaces the hand-maintained copy of ci.yml's steps that
tools/ci-parity.sh used to carry (ANTS-4392): local-gate.md § 3 requires the
local run to execute the pipeline's own definition, because a copy drifts and
a drifted copy returns green for a pipeline that will fail.

Every `run:` step is executed as ci.yml writes it, with the job's and the
step's `env`, its `working-directory` and its `if:`. What a GitHub runner
provides and this machine does not is mapped by the three tables below, and
ANYTHING they do not name is refused rather than guessed at — so a new action,
expression, key or shell in ci.yml stops the local run until someone decides
what it means here. One deliberate difference: step budgets (`timeout`) do not
apply here — see PREAMBLE — so a hang shows as a hang, not as a red step.

Usage:
  tools/ci_workflow.py jobs            # job ids in ci.yml, one per line
  tools/ci_workflow.py plan <job>      # what `run` would do, step by step
  tools/ci_workflow.py run <job>       # execute it in the repository root
  tools/ci_workflow.py docs-only       # paths on stdin; exit 0 if ci.yml's push
                                       # paths-ignore would skip them all
"""
import os
import re
import subprocess
import sys
import time

import yaml

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# CI_WORKFLOW_FILE exists for tests/features/ci_workflow_runner's fixtures.
WORKFLOW = os.environ.get("CI_WORKFLOW_FILE") or os.path.join(
    ROOT, ".github", "workflows", "ci.yml")

# `uses:` steps, by action (the part before `@`). Each stands for something
# the runner has to fetch and this machine already has.
ACTIONS = {
    "actions/checkout": "the working tree is the checkout",
    "awalsh128/cache-apt-pkgs-action":
        "packages come from this machine; the podman legs of "
        "tools/ci-parity.sh install ci.yml's own list",
    "actions/cache/restore": "the job's CCACHE_DIR persists between local runs",
    "actions/cache/save": "the job's CCACHE_DIR persists between local runs",
}

# The job's `env` applies as written, CCACHE_* included: they point ccache at
# ${{ github.workspace }}/.ccache (ignored by git), a cache separate from this
# machine's own, so ci.yml's 2G cap bounds that cache and not the developer's.
# Dropping them instead let a test that read CCACHE_MAXSIZE pass here and fail
# on GitHub (run 36105166384) — the one divergence this runner had chosen.

# What the GitHub runner's environment sets that ci.yml relies on without
# declaring. LC_ALL: the runner is C.UTF-8, and a dev box's Unicode collation
# has changed a test outcome (ANTS-2120). No display: the runner has none.
# No BASH_ENV: a non-interactive bash sources it whatever --noprofile/--norc
# say, and the runner leaves it unset (found by games-hub, relayed by UT_Ants).
RUNNER_ENV = {
    "CI": "true",
    "GITHUB_ACTIONS": "true",
    "GITHUB_WORKSPACE": ROOT,
    "RUNNER_OS": "Linux",
    "LC_ALL": os.environ.get("CI_PARITY_LOCALE", "C.UTF-8"),
}
RUNNER_UNSET = ("DISPLAY", "WAYLAND_DISPLAY", "BASH_ENV")

# Prepended to every step. The step budgets in ci.yml are sized for the
# GitHub runner, and a `timeout` that fires here SIGTERMs ninja, which
# corrupts its deps log. The command runs unbounded; everything else in the
# line is ci.yml's. GitHub's own bash flags follow it.
PREAMBLE = ('timeout() { if [[ "${1:-}" == -* ]]; then command timeout "$@"; '
            'else shift; "$@"; fi; }\n')
BASH = ["bash", "--noprofile", "--norc", "-eo", "pipefail", "-c"]

# Keys understood at each level. Anything else — defaults, strategy (matrix),
# needs, container, services, continue-on-error — changes what GitHub runs,
# so ignoring it would turn a divergence into a silent green.
WORKFLOW_KEYS = {"name", "on", True, "permissions", "concurrency", "env", "jobs"}
JOB_KEYS = {"name", "runs-on", "timeout-minutes", "env", "steps", "if"}

# Job-level `if:` values, each with its local meaning. ANTS-5343: build-asan
# is skipped on a GitHub push and runs nightly; locally the caller picked the
# job by name (the pre-push hook, ci-parity.sh), so it runs. Any other job
# condition is refused.
JOB_CONDITIONS = {
    "github.event_name != 'push'": "not on push — runs here because it was asked for",
}
STEP_KEYS = {"name", "uses", "with", "run", "env", "working-directory", "if",
             "shell"}

# Runner variables a step may read: the ones RUNNER_ENV sets. A step that
# reads any other GITHUB_* / RUNNER_* — including writing $GITHUB_ENV,
# $GITHUB_PATH or $GITHUB_OUTPUT for later steps — is refused.
RUNNER_VAR = re.compile(r"\$\{?((?:GITHUB|RUNNER)_[A-Z_]+)")


class Refused(Exception):
    pass


def load():
    with open(WORKFLOW, encoding="utf-8") as f:
        return yaml.safe_load(f)


def expand(text, where):
    """Substitute the one expression a run step may use; refuse the rest."""
    text = text.replace("${{ github.workspace }}", ROOT)
    left = re.search(r"\$\{\{[^}]*\}\}", text)
    if left:
        raise Refused(f"{where}: no local meaning for {left.group(0)}")
    return text


def unknown(node, allowed, where):
    extra = [str(k) for k in node if k not in allowed]
    if extra:
        raise Refused(f"{where}: no local meaning for key(s) {', '.join(extra)}")


def plan(job_id):
    """[(name, kind, detail, if_always, env, cwd)] for one job, or Refused."""
    wf = load()
    unknown(wf, WORKFLOW_KEYS, "workflow")
    jobs = wf.get("jobs", {})
    if job_id not in jobs:
        raise Refused(f"no job '{job_id}' in ci.yml (have: {', '.join(jobs)})")
    job = jobs[job_id]
    unknown(job, JOB_KEYS, job_id)
    job_cond = str(job.get("if", "")).strip()
    if job_cond and job_cond not in JOB_CONDITIONS:
        raise Refused(f"{job_id}: no local meaning for job if: {job_cond}")
    job_env = {k: expand(str(v), f"env.{k}")
               for k, v in (wf.get("env") or {}).items()}
    job_env.update({k: expand(str(v), f"{job_id}.env.{k}")
                    for k, v in (job.get("env") or {}).items()})
    steps = []
    for i, step in enumerate(job.get("steps") or []):
        name = step.get("name") or f"step {i + 1}"
        where = f"{job_id} / {name}"
        unknown(step, STEP_KEYS, where)
        cond = str(step.get("if", "")).strip()
        if cond not in ("", "always()"):
            raise Refused(f"{where}: no local meaning for if: {cond}")
        if "uses" in step:
            action = step["uses"].split("@")[0]
            if action not in ACTIONS:
                raise Refused(f"{where}: no local meaning for uses: {action}")
            steps.append((name, "skip", ACTIONS[action], cond == "always()",
                          {}, ROOT))
            continue
        if step.get("shell", "bash") != "bash":
            raise Refused(f"{where}: shell {step['shell']} is not bash")
        env = dict(job_env)
        env.update({k: expand(str(v), f"{where} env.{k}")
                    for k, v in (step.get("env") or {}).items()})
        cwd = os.path.join(ROOT, expand(step.get("working-directory", "."),
                                        where))
        for var in RUNNER_VAR.findall(step["run"]):
            if var not in RUNNER_ENV:
                raise Refused(f"{where}: no local meaning for ${var}")
        steps.append((name, "run", expand(step["run"], where),
                      cond == "always()", env, cwd))
    return steps


def run(job_id):
    steps = plan(job_id)
    base = {k: v for k, v in os.environ.items() if k not in RUNNER_UNSET}
    base.update(RUNNER_ENV)
    failed = []
    for name, kind, detail, always, env, cwd in steps:
        print(f"── {job_id} / {name}", flush=True)
        if kind == "skip":
            print(f"   ⊘ {detail}", flush=True)
            continue
        if failed and not always:
            print("   ⊘ skipped — an earlier step failed", flush=True)
            continue
        started = time.monotonic()
        rc = subprocess.run(BASH + [PREAMBLE + detail], cwd=cwd,
                            env={**base, **env}).returncode
        took = f"{time.monotonic() - started:.0f}s"
        if rc == 0:
            print(f"   ✓ {name} ({took})", flush=True)
        else:
            print(f"   ✗ {name} (exit {rc}, {took})", flush=True)
            failed.append(name)
    for name in failed:
        print(f"FAILED: {job_id} / {name}")
    return 1 if failed else 0


def glob_rx(pattern):
    """GitHub's path-filter glob: `**` crosses `/`, `*` and `?` do not."""
    out, i = "", 0
    while i < len(pattern):
        if pattern.startswith("**", i):
            out += ".*"
            i += 2
        elif pattern[i] == "*":
            out += "[^/]*"
            i += 1
        elif pattern[i] == "?":
            out += "[^/]"
            i += 1
        else:
            out += re.escape(pattern[i])
            i += 1
    return re.compile(out + r"\Z")


def docs_only(paths):
    """True when ci.yml's push trigger would skip every one of `paths`.

    tools/hooks/pre-push asks this instead of keeping its own copy of the list
    (ANTS-5322): a hand twin of paths-ignore is the drift local-gate.md § 3
    forbids.
    """
    on = load().get(True) or load().get("on") or {}
    ignore = (on.get("push") or {}).get("paths-ignore") or []
    if any(str(g).startswith("!") for g in ignore):
        raise Refused("no local meaning for a negated paths-ignore pattern")
    rxs = [glob_rx(str(g)) for g in ignore]
    return bool(paths) and bool(rxs) and all(
        any(rx.match(p) for rx in rxs) for p in paths)


def main(argv):
    if len(argv) == 2 and argv[1] == "docs-only":
        paths = [l.strip() for l in sys.stdin if l.strip()]
        try:
            return 0 if docs_only(paths) else 1
        except Refused as e:
            print(f"ci_workflow: refused — {e}", file=sys.stderr)
            return 3
    if len(argv) == 2 and argv[1] == "jobs":
        print("\n".join(load().get("jobs", {})))
        return 0
    if len(argv) != 3 or argv[1] not in ("plan", "run"):
        print(__doc__.strip().split("Usage:")[1], file=sys.stderr)
        return 2
    try:
        if argv[1] == "run":
            return run(argv[2])
        steps = plan(argv[2])
        cond = str(load()["jobs"][argv[2]].get("if", "")).strip()
        if cond:
            print(f"[job if] {cond}: {JOB_CONDITIONS[cond]}")
        for name, kind, detail, always, env, cwd in steps:
            print(f"[{kind}{' always' if always else ''}] {name}")
            if kind == "run":
                print(f"  cwd: {os.path.relpath(cwd, ROOT)}")
                for k, v in env.items():
                    print(f"  env: {k}={v}")
                for line in detail.rstrip("\n").split("\n"):
                    print(f"  | {line}")
            else:
                print(f"  {detail}")
        return 0
    except Refused as e:
        print(f"ci_workflow: refused — {e}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main(sys.argv))
