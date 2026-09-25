#!/usr/bin/env python3
"""ANTS-5322 — the local CI run executes ci.yml. See spec.md beside this."""
import os
import re
import subprocess
import sys
import tempfile

try:
    import yaml
except ImportError:
    print("SKIP: PyYAML not installed")
    sys.exit(77)

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
RUNNER = os.path.join(ROOT, "tools", "ci_workflow.py")
PARITY = os.path.join(ROOT, "tools", "ci-parity.sh")
HOST_JOBS = ("build-test", "build-asan", "cppcheck")
failures = []


def check(ok, msg):
    print(("PASS  " if ok else "FAIL  ") + msg)
    if not ok:
        failures.append(msg)


def runner(*args, workflow=None):
    env = dict(os.environ)
    env.pop("CI_WORKFLOW_FILE", None)
    # INV-3 asserts the WORKFLOW's CCACHE_* values reach the step. On GitHub
    # ci.yml's job env already puts CCACHE_MAXSIZE into this process, so the
    # caller's are cleared first: the fixture alone decides what the step sees,
    # here and there alike (run 36105166384 failed on exactly that difference).
    for k in [k for k in env if k.startswith("CCACHE_")]:
        del env[k]
    if workflow:
        env["CI_WORKFLOW_FILE"] = workflow
    p = subprocess.run([sys.executable, RUNNER, *args], env=env,
                       capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


with open(os.path.join(ROOT, ".github", "workflows", "ci.yml")) as f:
    ci = yaml.safe_load(f)

# INV-1
for job in HOST_JOBS:
    rc, out = runner("plan", job)
    check(rc == 0, f"INV-1 plan {job} exits 0 (got {rc}: {out[-300:]})")
    planned = [l[4:] for l in out.splitlines() if l.startswith("  | ")]
    runs = [s for s in ci["jobs"][job]["steps"] if "run" in s]
    check(out.count("[run") == len(runs),
          f"INV-1 {job}: {len(runs)} run steps planned")
    body = [l.replace("${{ github.workspace }}", ROOT)
            for s in runs for l in s["run"].rstrip("\n").split("\n")]
    check(planned == body, f"INV-1 {job}: every script line, in order, unchanged")

# INV-2
with open(PARITY) as f:
    code = [l for l in f if not l.lstrip().startswith("#")]
# A command followed by an argument; `cppcheck` alone is also a job id.
copied = re.compile(r"\b(ctest|cppcheck|cmake --build|appstreamcli|"
                    r"desktop-file-validate|groff|shellcheck)\s+[-\w./]")
hits = [l.strip() for l in code if copied.search(l)]
check(not hits, f"INV-2 ci-parity.sh runs no step itself (found: {hits})")
text = "".join(code)
claimed = re.findall(r"^(?:host|container)_jobs=\(([^)]*)\)", text, re.M)
claimed = " ".join(claimed).split()
for job in ci["jobs"]:
    check(job in claimed, f"INV-2 ci.yml job '{job}' is claimed by ci-parity.sh")

# INV-3
FIXTURE = """
env:
  WF_VAR: workflow
jobs:
  j:
    env:
      JOB_VAR: job
      CCACHE_MAXSIZE: 2G
    steps:
      - uses: actions/checkout@0000000000000000000000000000000000000000
      - name: env and cwd
        working-directory: tools
        env:
          STEP_VAR: step
        run: |
          test "$WF_VAR" = workflow
          test "$JOB_VAR" = job && test "$STEP_VAR" = step
          test "$CCACHE_MAXSIZE" = 2G
          test "$(basename "$PWD")" = tools
          test "$CI" = true && test "$LC_ALL" = C.UTF-8
          test -z "${DISPLAY:-}"
          timeout 1s sleep 2
          echo MARK-ENV-OK
      - name: fails
        run: exit 3
      - name: skipped after failure
        run: echo MARK-SKIPPED-RAN
      - name: always
        if: always()
        run: echo MARK-ALWAYS-RAN
"""
with tempfile.TemporaryDirectory() as tmp:
    wf = os.path.join(tmp, "ci.yml")
    with open(wf, "w") as f:
        f.write(FIXTURE)
    os.environ["DISPLAY"] = ":99"
    benv = os.path.join(tmp, "benv.sh")
    with open(benv, "w") as f:
        f.write("exit 7\n")
    os.environ["BASH_ENV"] = benv
    rc, out = runner("run", "j", workflow=wf)
    check("MARK-ENV-OK" in out,
          "INV-3 workflow/job/step env incl. CCACHE_, cwd, CI/LC_ALL, "
          "no DISPLAY, no BASH_ENV, timeout shim")
    check("MARK-SKIPPED-RAN" not in out, "INV-3 a step after a failure is skipped")
    check("MARK-ALWAYS-RAN" in out, "INV-3 an if: always() step still runs")
    check(rc != 0, "INV-3 a failing step fails the run")

    # INV-4
    ok_steps = "    steps:\n      - run: true\n"
    for label, text in (
            ("unknown action",
             "jobs:\n  j:\n    steps:\n      - uses: someone/new-action@v1\n"),
            ("unknown expression",
             "jobs:\n  j:\n    steps:\n      - run: echo ${{ github.sha }}\n"),
            ("unknown if",
             "jobs:\n  j:\n    steps:\n      - if: success()\n        run: true\n"),
            ("workflow defaults",
             "defaults:\n  run:\n    shell: sh\njobs:\n  j:\n" + ok_steps),
            ("job strategy (matrix)",
             "jobs:\n  j:\n    strategy:\n      matrix:\n        a: [1]\n" + ok_steps),
            ("job container",
             "jobs:\n  j:\n    container: ubuntu:22.04\n" + ok_steps),
            ("step continue-on-error",
             "jobs:\n  j:\n    steps:\n      - run: true\n        continue-on-error: true\n"),
            ("write to $GITHUB_ENV",
             "jobs:\n  j:\n    steps:\n      - run: echo A=1 >> \"$GITHUB_ENV\"\n"),
            ("unset runner var",
             "jobs:\n  j:\n    steps:\n      - run: echo ${RUNNER_TEMP}\n")):
        with open(wf, "w") as f:
            f.write(text)
        rc, out = runner("plan", "j", workflow=wf)
        check(rc == 3 and "refused" in out, f"INV-4 {label} is refused")

print(f"\n{len(failures)} failure(s)")
sys.exit(1 if failures else 0)
