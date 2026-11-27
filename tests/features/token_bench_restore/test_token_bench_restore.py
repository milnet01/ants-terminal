#!/usr/bin/env python3
"""ANTS-6630 — the bench's bug test sees only the starting tests/. See spec.md
beside this."""
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
BENCH = os.path.join(ROOT, "tools", "token-bench", "bench.py")
failures = []

if shutil.which("git") is None:
    print("SKIP: git not installed")
    sys.exit(77)

# A shell or Python test outside a bundle main scrubs the git environment
# itself; otherwise `git init` lands on the caller's repo (ANTS-3841).
for var in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR"):
    os.environ.pop(var, None)

spec = importlib.util.spec_from_file_location("bench", BENCH)
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


def check(ok, msg):
    print(("PASS  " if ok else "FAIL  ") + msg)
    if not ok:
        failures.append(msg)


def git(cwd, *args):
    subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@localhost",
                    *args], cwd=cwd, check=True, capture_output=True)


# INV-1
with tempfile.TemporaryDirectory(prefix="token-bench-") as tmp:
    work = Path(tmp)
    (work / "tests").mkdir()
    (work / "tests" / "test_x.txt").write_text("start\n")
    git(work, "init", "-q")
    git(work, "add", "-A")
    git(work, "commit", "-qm", "start")
    start = subprocess.run(["git", "rev-parse", "HEAD"], cwd=work, check=True,
                           capture_output=True, text=True).stdout.strip()
    # The run edits the tracked test and adds a file beside it.
    (work / "tests" / "test_x.txt").write_text("edited\n")
    (work / "tests" / "conftest.py").write_text("# added by the run\n")
    bug_test = ("sh -c 'grep -qx start tests/test_x.txt"
                " && test ! -e tests/conftest.py'")
    passed = bench.bug_test_passes(work, start, bug_test)
    check(passed, "INV-1 the bug test sees the starting tests/: the edit is "
                  "undone and the added conftest.py is gone")

print(f"\n{len(failures)} failure(s)")
sys.exit(1 if failures else 0)
