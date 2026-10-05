#!/usr/bin/env python3
"""ANTS-3428 — flag a below-latest dependency pin that has no Downgrade Ledger row.

docs/standards/dependencies.md § 2 makes an undocumented below-latest pin a
defect, and § 3's ledger says when a held pin is due for a retest. This reads
the mechanical pins (GitHub action SHAs with their `# vX.Y.Z` comment, and
FetchContent GIT_TAGs on github.com), asks GitHub for each one's latest
release, and checks both rules. Contract: tests/features/dependency_pins/spec.md.

Usage:
  tools/check-dependency-pins.py            # check, asking GitHub through `gh`
  tools/check-dependency-pins.py --list     # print the pins read, no network
  --root <dir>      scan another tree (default: the repository)
  --upstream <json> take upstream answers from a file instead of the network

Exit: 0 all checked, nothing found; 1 a finding; 2 no finding, but a pin
could not be checked (each is named).

Runs daily in .github/workflows/release-audit.yml. Not in the pre-push gate:
it needs the network, and an upstream release would block unrelated pushes.
"""
import argparse
import glob
import json
import os
import re
import subprocess
import sys

USES_RE = re.compile(r"^\s*-?\s*uses:\s*(\S+)(?:\s+#\s*(\S+))?")
SHA_RE = re.compile(r"^[0-9a-f]{40}$")
FETCH_RE = re.compile(
    r"FetchContent_Declare\(\s*\w+\s+GIT_REPOSITORY\s+https://github\.com/"
    r"([\w.-]+/[\w.-]+?)(?:\.git)?\s+GIT_TAG\s+(\S+?)\s*\)", re.S)


def read_pins(root):
    """Return (pins, findings). A pin is {repo, where, version, sha}."""
    pins, findings = [], []
    for wf in sorted(glob.glob(os.path.join(root, ".github", "workflows", "*.yml"))):
        rel = os.path.relpath(wf, root)
        with open(wf, encoding="utf-8") as f:
            for n, line in enumerate(f, 1):
                m = USES_RE.match(line)
                if not m:
                    continue
                ref, comment = m.group(1), m.group(2)
                if ref.startswith("./") or ref.startswith("docker://"):
                    continue
                action, _, at = ref.partition("@")
                repo = "/".join(action.split("/")[:2])
                where = f"{rel}:{n}"
                if not SHA_RE.match(at) or not comment:
                    findings.append(f"{repo}: cannot read a version at {where} — pin a "
                                    "40-hex SHA with a '# vX.Y.Z' comment")
                    continue
                pins.append({"repo": repo, "where": where, "version": comment, "sha": at})
    for cm in [os.path.join(root, "CMakeLists.txt")] + sorted(
            glob.glob(os.path.join(root, "cmake", "*.cmake"))):
        if not os.path.exists(cm):
            continue
        with open(cm, encoding="utf-8") as f:
            text = f.read()
        for m in FETCH_RE.finditer(text):
            line = text.count("\n", 0, m.start()) + 1
            pins.append({"repo": m.group(1), "where": f"{os.path.relpath(cm, root)}:{line}",
                         "version": m.group(2), "sha": None})
    return pins, findings


def read_ledger(root):
    """Rows of dependencies.md § 3, as dicts; placeholder rows dropped."""
    path = os.path.join(root, "docs", "standards", "dependencies.md")
    rows, in_section = [], False
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("## "):
                in_section = "Downgrade Ledger" in line
                continue
            if not in_section or not line.startswith("|"):
                continue
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            if len(cells) < 7 or cells[0] in ("Dependency",) or set(cells[0]) <= set("-"):
                continue
            if cells[0].startswith("_("):
                continue
            rows.append({"name": cells[0].strip("`").lower(), "latest_tested": cells[2],
                         "trigger": cells[6], "raw": line.strip()})
    return rows


def row_for(repo, rows):
    short = repo.split("/")[-1].lower()
    for r in rows:
        if r["name"] in (repo.lower(), short):
            return r
    return None


def version_key(tag):
    return tuple(int(x) for x in re.findall(r"\d+", tag))


def newer(a, b):
    """True if tag a is newer than tag b; None if they cannot be compared."""
    ka, kb = version_key(a), version_key(b)
    if not ka or not kb:
        return None
    return ka > kb


class Upstream:
    def __init__(self, path):
        self.data = None
        if path:
            with open(path, encoding="utf-8") as f:
                self.data = json.load(f)
        self.cache = {}

    def _gh(self, *args):
        try:
            p = subprocess.run(["gh", "api", *args], capture_output=True, text=True,
                               timeout=60)
        except (OSError, subprocess.TimeoutExpired):
            return None
        return p.stdout.strip() if p.returncode == 0 and p.stdout.strip() else None

    def latest(self, repo):
        if self.data is not None:
            return self.data.get(repo, {}).get("latest")
        if repo not in self.cache:
            self.cache[repo] = self._gh(f"repos/{repo}/releases/latest", "--jq", ".tag_name")
        return self.cache[repo]

    def tag_sha(self, repo, tag):
        if self.data is not None:
            return self.data.get(repo, {}).get("tags", {}).get(tag)
        key = (repo, tag)
        if key not in self.cache:
            self.cache[key] = self._gh(f"repos/{repo}/commits/{tag}", "--jq", ".sha")
        return self.cache[key]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", default=os.path.abspath(
        os.path.join(os.path.dirname(__file__), "..")))
    ap.add_argument("--upstream")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()

    pins, findings = read_pins(args.root)
    if args.list:
        for p in pins:
            print(f"{p['repo']}  {p['version']}  {p['where']}")
        return 0

    rows = read_ledger(args.root)
    for r in rows:
        if not r["latest_tested"] or not r["trigger"]:
            findings.append(f"{r['name']}: ledger row is malformed (empty Latest tested or "
                            f"Re-test trigger): {r['raw']}")
    up = Upstream(args.upstream)
    unchecked, ok = [], []
    for p in pins:
        repo, ver, where = p["repo"], p["version"], p["where"]
        latest = up.latest(repo)
        if latest is None:
            unchecked.append(f"{repo} {ver} ({where}): no latest release from GitHub")
            continue
        if p["sha"]:
            sha = up.tag_sha(repo, ver)
            if sha is None:
                unchecked.append(f"{repo} {ver} ({where}): tag commit not resolved")
            elif sha != p["sha"]:
                findings.append(f"{repo}: SHA at {where} is not the commit {ver} names "
                                f"({p['sha'][:12]} vs {sha[:12]})")
        is_newer = newer(latest, ver)
        if is_newer is None:
            findings.append(f"{repo}: could not compare pinned {ver} with latest {latest} "
                            f"({where})")
            continue
        row = row_for(repo, rows)
        if not is_newer:
            ok.append(f"{repo} {ver} is the latest")
        elif row is None:
            findings.append(f"{repo}: {ver} at {where} is behind latest {latest}, and "
                            "dependencies.md § 3 has no ledger row for it")
        elif row["latest_tested"] and newer(latest, row["latest_tested"]):
            findings.append(f"{repo}: held at {ver}; {latest} is newer than the ledger's "
                            f"Latest tested {row['latest_tested']} — retest due")
        else:
            ok.append(f"{repo} {ver} held by its ledger row (latest {latest})")

    for line in dict.fromkeys(ok):  # one line per pin, however many times it is used
        print(f"OK: {line}")
    for line in unchecked:
        print(f"UNCHECKED: {line}")
    for line in findings:
        print(f"FINDING: {line}")
    if findings:
        return 1
    return 2 if unchecked else 0


if __name__ == "__main__":
    sys.exit(main())
