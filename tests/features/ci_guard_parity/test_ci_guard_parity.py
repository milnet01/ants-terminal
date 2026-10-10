#!/usr/bin/env python3
"""The podman compile guard configures as ci.yml does. See spec.md beside this."""
import os
import re
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


def configure_flags(text):
    """The -D cache variables and the -G generator, as a set of strings."""
    flags = set(re.findall(r"-D[A-Za-z0-9_]+(?::[A-Z]+)?=\S+", text))
    gen = re.search(r"-G\s+(\S+)", text)
    if gen:
        flags.add("-G " + gen.group(1))
    return flags


with open(os.path.join(ROOT, ".github", "workflows", "ci.yml")) as f:
    ci = yaml.safe_load(f)

# Code lines only: the header discusses commands it does not run.
with open(GUARD) as f:
    code = "\n".join(l for l in f.read().splitlines()
                     if not l.lstrip().startswith("#"))

# The compile leg: from its `bash -euo pipefail -c '` to the closing quote.
m = re.search(r"podman run (?:(?!podman run).)*?bash -euo pipefail -c '(.*?)'",
              code, re.S)
check(m is not None, "the guard's compile block is found")
if not m:
    sys.exit(1)
compile_cmd = m.group(1)
compile_run = code[code.rfind("podman run", 0, m.start(1)):m.start(1)]
guard_flags = configure_flags(compile_cmd)
guard_env = dict(re.findall(r"-e (CCACHE_[A-Z_]+)=(\S+)", compile_run))

for job in GUARDED_JOBS:
    spec = ci["jobs"][job]
    steps = [s for s in spec["steps"] if s.get("name", "").startswith("Configure")]
    check(len(steps) == 1, f"{job} has one Configure step")
    if len(steps) != 1:
        continue
    want = configure_flags(steps[0]["run"])
    check(want == guard_flags,
          f"INV-1 {job}: configure flags match "
          f"(missing {sorted(want - guard_flags)}, extra {sorted(guard_flags - want)})")
    want_env = {k: str(v) for k, v in (spec.get("env") or {}).items()
                if k.startswith("CCACHE_") and k != "CCACHE_DIR"}
    diff = {k: (v, guard_env.get(k)) for k, v in want_env.items()
            if guard_env.get(k) != v}
    check(not diff, f"INV-2 {job}: CCACHE_* env matches (ci.yml vs guard: {diff})")

sys.exit(1 if failures else 0)
