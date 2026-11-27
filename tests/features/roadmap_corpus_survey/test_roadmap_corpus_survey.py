#!/usr/bin/env python3
"""ANTS-6630 — the corpus survey copes with no items. See spec.md beside this."""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
SCRIPT = os.path.join(ROOT, "tools", "roadmap-corpus-survey.py")
failures = []


def check(ok, msg):
    print(("PASS  " if ok else "FAIL  ") + msg)
    if not ok:
        failures.append(msg)


# INV-1
with tempfile.TemporaryDirectory(prefix="corpus-survey-") as tmp:
    project = os.path.join(tmp, "empty")
    os.makedirs(project)
    with open(os.path.join(project, "ROADMAP.md"), "w", encoding="utf-8") as f:
        f.write("# Roadmap\n\nNothing planned yet.\n")
    p = subprocess.run([sys.executable, SCRIPT, project],
                       capture_output=True, text=True, timeout=60)
    out = p.stdout + p.stderr
    check(p.returncode == 0 and "field absence" in out
          and "ZeroDivisionError" not in out,
          f"INV-1 a roadmap with no items exits 0 (rc={p.returncode}: {out[-300:]})")

print(f"\n{len(failures)} failure(s)")
sys.exit(1 if failures else 0)
