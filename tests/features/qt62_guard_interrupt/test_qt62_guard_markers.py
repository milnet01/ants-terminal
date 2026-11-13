#!/usr/bin/env python3
"""tools/qt62-guard.sh run against a stub podman. See spec.md, INV-6 and INV-7."""
import os
import shutil
import subprocess
import sys
import tempfile
import time

try:
    import yaml  # noqa: F401 — the guard's ci_workflow.py needs it
except ImportError:
    print("SKIP: PyYAML not installed")
    sys.exit(77)
if not shutil.which("flock"):
    print("SKIP: flock not installed")
    sys.exit(77)

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
GUARD = os.path.join(ROOT, "tools", "qt62-guard.sh")
failures = []

# The stub answers "cached and idle" to every query, logs each `run`, and ends
# a run with STUB_RUN_RC once the file STUB_RUN_WAIT names is gone.
STUB = """#!/usr/bin/env bash
case "$1 $2" in
    "container inspect") echo false; exit 0 ;;
esac
if [[ "$1" == "run" ]]; then
    echo run >> "$STUB_LOG"
    while [[ -n "${STUB_RUN_WAIT:-}" && -e "$STUB_RUN_WAIT" ]]; do sleep 0.05; done
    exit "${STUB_RUN_RC:-0}"
fi
exit 0
"""


def check(ok, msg):
    print(("PASS  " if ok else "FAIL  ") + msg)
    if not ok:
        failures.append(msg)


class Bench:
    """One temp home for the stub, the marker directory and the logs."""

    def __init__(self, tmp):
        self.tmp = tmp
        os.mkdir(os.path.join(tmp, "bin"))
        stub = os.path.join(tmp, "bin", "podman")
        with open(stub, "w") as f:
            f.write(STUB)
        os.chmod(stub, 0o755)
        self.state = os.path.join(tmp, "cache", "ants-terminal", "qt62-guard")

    def env(self, name, rc=0, wait=None):
        e = dict(os.environ)
        e["PATH"] = os.path.join(self.tmp, "bin") + os.pathsep + e["PATH"]
        e["XDG_CACHE_HOME"] = os.path.join(self.tmp, "cache")
        # Keep the compile cache directory the guard creates out of the checkout.
        e["GIT_CONFIG_COUNT"] = "1"
        e["GIT_CONFIG_KEY_0"] = "ants.qt62CcacheDir"
        e["GIT_CONFIG_VALUE_0"] = os.path.join(self.tmp, "ccache")
        e["STUB_LOG"] = os.path.join(self.tmp, name + ".log")
        e["STUB_RUN_RC"] = str(rc)
        if wait:
            e["STUB_RUN_WAIT"] = wait
        return e

    def runs(self, name):
        path = os.path.join(self.tmp, name + ".log")
        return open(path).read().count("run") if os.path.exists(path) else 0

    def markers(self):
        if not os.path.isdir(self.state):
            return []
        return [n for n in os.listdir(self.state) if n.endswith(".interrupted")]


def guard(env):
    return subprocess.run([GUARD], env=env, capture_output=True, text=True, timeout=120)


# INV-7 — the marker goes when the job finished, and stays when it was killed.
for rc, kept, what in ((0, False, "a pass"), (1, False, "a job failure"),
                       (137, True, "an OOM kill"), (125, True, "podman's own failure")):
    with tempfile.TemporaryDirectory() as tmp:
        b = Bench(tmp)
        r = guard(b.env("a", rc=rc))
        check(b.runs("a") == 1, f"INV-7 the stub container ran for {what}")
        check((r.returncode == 0) == (rc == 0),
              f"INV-7 exit {r.returncode} after {what} (container exit {rc})")
        check(bool(b.markers()) == kept,
              f"INV-7 after {what} the marker is {'kept' if kept else 'removed'}"
              f" (found {b.markers()})")
        if kept:
            check("FAILED to build" not in r.stderr,
                  f"INV-7 {what} is not reported as a compile failure")

# INV-6 — a second run, started while the first holds the tree and before any
# container is visible, runs nothing and leaves the first run's marker.
with tempfile.TemporaryDirectory() as tmp:
    b = Bench(tmp)
    gate = os.path.join(tmp, "gate")
    open(gate, "w").close()
    first = subprocess.Popen([GUARD], env=b.env("a", wait=gate),
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    deadline = time.time() + 60
    while b.runs("a") == 0 and time.time() < deadline:
        time.sleep(0.05)
    check(b.runs("a") == 1, "INV-6 the first run reached its container")
    second = guard(b.env("b", rc=125))
    check(b.runs("b") == 0, f"INV-6 the second run started no container (ran {b.runs('b')})")
    check(second.returncode == 1, f"INV-6 the second run exits 1 (got {second.returncode})")
    check("still running" in second.stdout + second.stderr,
          "INV-6 the second run says the first is still running")
    check(bool(b.markers()), "INV-6 the first run's marker survives the second run")
    warm = subprocess.run([GUARD, "--warm-only"], env=b.env("c"),
                          capture_output=True, text=True, timeout=120)
    check(warm.returncode == 0 and b.runs("c") == 0,
          f"INV-6 --warm-only skips with exit 0 (got {warm.returncode}, ran {b.runs('c')})")
    os.remove(gate)
    out, err = first.communicate(timeout=60)
    check(first.returncode == 0, f"INV-6 the first run then passes (got {first.returncode})")
    check(not b.markers(), "INV-6 the first run removes its own marker")

if failures:
    print(f"\n{len(failures)} failure(s)")
    sys.exit(1)
print("\nall checks passed")
