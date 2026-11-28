#!/usr/bin/env python3
"""ANTS-3428 — a below-latest pin needs a ledger row. See spec.md beside this."""
import json
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
SCRIPT = os.path.join(ROOT, "tools", "check-dependency-pins.py")
failures = []

SHA_A = "a" * 40
SHA_B = "b" * 40
LEDGER_HEAD = (
    "## 3. Downgrade Ledger\n\n"
    "| Dependency | Pinned at | Latest tested | First broken version "
    "| What breaks (symptom + link) | Logged | Re-test trigger |\n"
    "|---|---|---|---|---|---|---|\n")


def check(ok, msg):
    print(("PASS  " if ok else "FAIL  ") + msg)
    if not ok:
        failures.append(msg)


def tree(workflow, ledger_rows="| _(none — no active downgrades)_ | | | | | | |\n",
         gtest_tag="v1.18.0"):
    d = tempfile.mkdtemp(prefix="deppins-")
    os.makedirs(os.path.join(d, ".github", "workflows"))
    os.makedirs(os.path.join(d, "docs", "standards"))
    with open(os.path.join(d, ".github", "workflows", "ci.yml"), "w") as f:
        f.write(workflow)
    with open(os.path.join(d, "CMakeLists.txt"), "w") as f:
        f.write("FetchContent_Declare(googletest\n"
                "    GIT_REPOSITORY https://github.com/google/googletest.git\n"
                f"    GIT_TAG        {gtest_tag})\n")
    with open(os.path.join(d, "docs", "standards", "dependencies.md"), "w") as f:
        f.write("# Deps\n\n" + LEDGER_HEAD + ledger_rows + "\n## 4. Floors\n")
    return d


UPSTREAM = {
    "actions/checkout": {"latest": "v7.0.1", "tags": {"v7.0.1": SHA_A, "v6.0.3": SHA_B}},
    "google/googletest": {"latest": "v1.18.0", "tags": {}},
}


def run(root, upstream=UPSTREAM, *extra):
    up = os.path.join(root, "upstream.json")
    with open(up, "w") as f:
        json.dump(upstream, f)
    p = subprocess.run([sys.executable, SCRIPT, "--root", root, "--upstream", up, *extra],
                       capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def uses(ref, comment):
    return f"jobs:\n  a:\n    steps:\n      - uses: actions/checkout@{ref}  # {comment}\n"


if not os.path.exists(SCRIPT):
    check(False, "tools/check-dependency-pins.py exists")
    sys.exit(1)

# INV-1 — the real tree's pins are all read.
p = subprocess.run([sys.executable, SCRIPT, "--list"], capture_output=True, text=True, cwd=ROOT)
for repo in ("actions/checkout", "actions/cache", "awalsh128/cache-apt-pkgs-action",
             "google/googletest"):
    check(repo in p.stdout, f"INV-1 --list on the real tree names {repo}")

# Clean: at latest, SHA matches → exit 0, no finding.
rc, out = run(tree(uses(SHA_A, "v7.0.1")))
check(rc == 0 and "FINDING" not in out, f"clean tree exits 0 (rc={rc})")

# INV-1 — local and docker refs are skipped.
wf = uses(SHA_A, "v7.0.1") + "      - uses: ./.github/actions/x\n      - uses: docker://alpine:3\n"
rc, out = run(tree(wf))
check(rc == 0 and "FINDING" not in out, f"INV-1 local and docker refs skipped (rc={rc})")

# INV-1 — a mutable tag is a finding.
rc, out = run(tree("jobs:\n  a:\n    steps:\n      - uses: actions/checkout@v7\n"))
check(rc == 1 and "cannot read" in out, f"INV-1 unpinned uses: is a finding (rc={rc})")

# INV-2 — behind with no ledger row.
rc, out = run(tree(uses(SHA_B, "v6.0.3")))
check(rc == 1 and "behind" in out and "actions/checkout" in out,
      f"INV-2 behind with no row is a finding (rc={rc})")
rc, out = run(tree(uses(SHA_A, "v7.0.1"), gtest_tag="v1.15.2"))
check(rc == 1 and "google/googletest" in out, f"INV-2 FetchContent behind is a finding (rc={rc})")

# INV-2 — tags with nothing to compare.
up = dict(UPSTREAM, **{"actions/checkout": {"latest": "stable", "tags": {"stable": SHA_A}}})
rc, out = run(tree(uses(SHA_A, "nightly")), up)
check(rc == 1 and "could not compare" in out, f"INV-2 incomparable tags are a finding (rc={rc})")

# INV-3 — a row suppresses INV-2 while latest == latest tested.
row = "| actions/checkout | v6.0.3 | v7.0.1 | v7.0.0 | breaks x | 2026-10-05 | re-test when > v7.0.1 |\n"
rc, out = run(tree(uses(SHA_B, "v6.0.3"), row))
check(rc == 0 and "FINDING" not in out, f"INV-3 a ledger row suppresses behind (rc={rc})")

# INV-3 — a newer release makes the row due.
row = "| `checkout` | v6.0.3 | v7.0.0 | v7.0.0 | breaks x | 2026-10-05 | re-test when > v7.0.0 |\n"
rc, out = run(tree(uses(SHA_B, "v6.0.3"), row))
check(rc == 1 and "retest" in out, f"INV-3 newer than Latest tested is due (rc={rc})")

# INV-3 — a malformed row.
row = "| actions/checkout | v6.0.3 | v7.0.1 | v7.0.0 | breaks x | 2026-10-05 | |\n"
rc, out = run(tree(uses(SHA_B, "v6.0.3"), row))
check(rc == 1 and "malformed" in out, f"INV-3 a row with no trigger is malformed (rc={rc})")

# INV-4 — the SHA must be the tag's commit.
rc, out = run(tree(uses(SHA_B, "v7.0.1")))
check(rc == 1 and "SHA" in out, f"INV-4 SHA mismatch is a finding (rc={rc})")

# INV-5 — a pin the upstream file does not know is unchecked, exit 2.
rc, out = run(tree(uses(SHA_A, "v7.0.1")), {"actions/checkout": UPSTREAM["actions/checkout"]})
check(rc == 2 and "UNCHECKED" in out and "google/googletest" in out,
      f"INV-5 an unchecked pin exits 2 and is named (rc={rc})")

# INV-7 — the daily audit runs it.
with open(os.path.join(ROOT, ".github", "workflows", "release-audit.yml")) as f:
    audit = f.read()
check("tools/check-dependency-pins.py" in audit and "GH_TOKEN" in audit,
      "INV-7 release-audit.yml runs the script with GH_TOKEN")


def step_script(yml, name):
    """The `run: |` body of the step called `name`, dedented."""
    lines = yml.split("\n")
    i = next(k for k, l in enumerate(lines) if l.strip() == f"- name: {name}")
    while lines[i].strip() != "run: |":
        i += 1
    indent = len(lines[i]) - len(lines[i].lstrip()) + 2
    body = []
    for l in lines[i + 1:]:
        if l.strip() and len(l) - len(l.lstrip()) < indent:
            break
        body.append(l[indent:])
    return "\n".join(body)


def run_step(script, stubs, env, cwd=ROOT):
    """Run a step as GitHub runs one with no shell: key (`bash -e`), with the
    named commands replaced by stub scripts on PATH."""
    d = tempfile.mkdtemp(prefix="audit-step-")
    for cmd, body in stubs.items():
        path = os.path.join(d, cmd)
        with open(path, "w") as f:
            f.write("#!/bin/sh\n" + body)
        os.chmod(path, 0o755)
    full = dict(os.environ, PATH=d + os.pathsep + os.environ["PATH"], **env)
    p = subprocess.run(["bash", "-e", "-c", script], env=full, cwd=cwd,
                       capture_output=True, text=True, timeout=60)
    return p.returncode, p.stdout + p.stderr


# INV-8 — under GitHub's default `bash -e`, the pins step still reads the
# script's exit 2 as a warning.
rc, out = run_step(step_script(audit, "Check dependency pins against upstream"),
                   {"python3": "echo UNCHECKED x; exit 2\n"}, {"GH_TOKEN": "t"})
check(rc == 0 and "some pins could not be checked" in out,
      f"INV-8 the pins step turns exit 2 into a warning under bash -e (rc={rc})")

# INV-8 — the audit step reports an unfetchable permanent link as unchecked and
# a listing with no package as an error, rather than dying on the first one.
GH = ("case \"$1 $2\" in\n"
      "  'release list') echo v1.2.3 ;;\n"
      "  'release view') printf '%s\\n' Ants_Terminal-1.2.3-x86_64.AppImage "
      "Ants_Terminal-1.2.3-x86_64.AppImage.zsync Ants_Terminal-x86_64.AppImage ;;\n"
      "esac\n")
CURL = ("for a; do case \"$a\" in *releases/latest/*) exit 6 ;; esac; done\n"
        "echo '<html>no packages here</html>'\n")
rc, out = run_step(step_script(audit, "Audit published downloads"),
                   {"gh": GH, "curl": CURL},
                   {"GH_TOKEN": "t", "REPO": "o/r", "OBS_BASE": "https://obs.invalid",
                    "REHEARSAL": "false"})
check(rc == 1 and "not checked" in out and "publishes no ants-terminal package" in out,
      f"INV-8 the audit step reports each failure under bash -e (rc={rc}: {out[-300:]})")

# INV-9 — release.yml takes each linuxdeploy AppImage from a tagged release and
# checks its sha256 before anything runs it. The stub curl logs each URL and
# writes junk, so the real sha256sum must refuse both files and stop the step.
with open(os.path.join(ROOT, ".github", "workflows", "release.yml")) as f:
    release = f.read()
work = tempfile.mkdtemp(prefix="linuxdeploy-step-")
urls = os.path.join(work, "urls")
CURL = ("out=; for a; do case \"$a\" in http*) echo \"$a\" >> \"$URLS\" ;; esac;"
        " [ \"$prev\" = -o ] && out=$a; prev=$a; done\necho junk > \"$out\"\n")
name = next(l.strip()[len("- name: "):] for l in release.split("\n")
            if l.strip().startswith("- name: Download linuxdeploy"))
rc, out = run_step(step_script(release, name), {"curl": CURL}, {"URLS": urls}, cwd=work)
got = open(urls).read().split() if os.path.exists(urls) else []
pinned = re.compile(r"^https://github\.com/linuxdeploy/[^/]+/releases/download/"
                    r"(?!continuous/|latest/)[^/]+/linuxdeploy[\w-]*-x86_64\.AppImage$")
check(len(got) == 2 and all(pinned.match(u) for u in got),
      f"INV-9 both linuxdeploy downloads name a tagged release ({got})")
check(rc != 0 and "linuxdeploy: FAILED" in out and "linuxdeploy-plugin-qt: FAILED" in out,
      f"INV-9 a download with the wrong sha256 stops the step (rc={rc}: {out[-300:]})")

print(f"\n{len(failures)} failure(s)")
sys.exit(1 if failures else 0)
