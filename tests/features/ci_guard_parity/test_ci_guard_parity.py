#!/usr/bin/env python3
"""The podman guard runs ci.yml's own job steps. See spec.md beside this."""
import os
import re
import subprocess
import sys

try:
    import yaml
except ImportError:
    print("SKIP: PyYAML not installed")
    sys.exit(77)

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
GUARD = os.path.join(ROOT, "tools", "qt62-guard.sh")
GUARDED_JOBS = ("qt62-baseline", "build-test")
failures = []


def check(ok, msg):
    print(("PASS  " if ok else "FAIL  ") + msg)
    if not ok:
        failures.append(msg)


with open(os.path.join(ROOT, ".github", "workflows", "ci.yml")) as f:
    ci = yaml.safe_load(f)

# Code lines only: the header discusses commands it does not run.
with open(GUARD) as f:
    code = "\n".join(l for l in f.read().splitlines()
                     if not l.lstrip().startswith("#"))

# INV-1 — no configure or build line of the guard's own. An `echo` line is a
# message that may name the command, not a run of it.
commands = "\n".join(l for l in code.splitlines() if not l.lstrip().startswith("echo "))
own = re.findall(r"\bcmake\s+(?:-S|-B|--build)\b[^\n]*", commands)
check(not own, f"INV-1 the guard runs no cmake of its own (found {own})")

# INV-2 — one container run, and it runs the job through ci_workflow.py.
runs = [m.start() for m in re.finditer(r"\bpodman run\b", code)]
check(len(runs) == 1, f"INV-2 the guard has one podman run (found {len(runs)})")
if runs:
    block = code[runs[0]:code.find("&\n", runs[0])]
    check('tools/ci_workflow.py run "$job"' in block,
          "INV-2 that run executes tools/ci_workflow.py run \"$job\"")

# INV-3 — --print mounts the build volume at the job's own Configure -B dir.
for job in GUARDED_JOBS:
    steps = [s for s in ci["jobs"][job]["steps"]
             if s.get("name", "").startswith("Configure")]
    check(len(steps) == 1, f"{job} has one Configure step")
    if len(steps) != 1:
        continue
    want = re.search(r"-B\s+(\S+)", steps[0]["run"])
    check(want is not None, f"{job}'s Configure step names a -B directory")
    out = subprocess.run(["bash", GUARD, "--job", job, "--print"], cwd=ROOT,
                         capture_output=True, text=True)
    got = re.search(r"^volume: .*\(at /src/(\S+)\)$", out.stdout, re.M)
    check(out.returncode == 0 and got is not None,
          f"INV-3 {job}: --print names the volume's mount (rc {out.returncode}, "
          f"stderr {out.stderr.strip()!r})")
    if want and got:
        check(got.group(1) == want.group(1),
              f"INV-3 {job}: mounted at /src/{got.group(1)}, ci.yml builds in "
              f"{want.group(1)}")

sys.exit(1 if failures else 0)
