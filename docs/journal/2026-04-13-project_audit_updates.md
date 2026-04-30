# Project Audit — Improvement Brief (historical, 2026-04-14)

> **Historical snapshot, not current.** Targets the long-superseded
> `ants-audit` CLI v0.6.3. The current audit pipeline lives inside
> `auditdialog.cpp` (in-process, no external CLI). Relocated from
> `/project_audit_updates.md` to `docs/journal/` per ANTS-1121 theme T3.

## Original brief (2026-04-14)

**Target:** `ants-audit` CLI (currently v0.6.3), used to generate the reports at `/tmp/ants-audit-*.txt` that a Claude Code session then consumes to fix issues.

**Context:** I audited a 2026-04-14 run of the tool against a C++ engine (~108K LOC) and compared its output against a manually-run 5-phase codebase audit. This brief lists five concrete classes of bug in the tool (and its consumer prompt), with on-disk evidence, the proposed fix, and how to verify each. Ship them one-by-one or bundled — each is independent.

The evidence quoted below is from an actual report file on the reviewer's machine. You don't need to access that file to implement these fixes; the pattern samples are reproduced inline.

---

## Issue 1 — Git conflict-marker pattern matches ASCII underlines

**Evidence.** In the latest run, the category `--- [BLOCKER] Git Conflict Markers — 26 finding(s) ---` flagged lines like:

```
./external/dr_libs/dr_wav.h:12:============
./external/dr_libs/dr_wav.h:88:=============
./external/dr_libs/dr_wav.h:102:========================
./external/dr_libs/dr_wav.h:9016:===============================================================================
```

These are section-heading underlines in a vendored single-header audio library's doc comments — not merge conflict markers. A real git conflict marker is *exactly* 7 characters at the start of a line: `<<<<<<<`, `|||||||`, `=======`, `>>>>>>>`. Ten-plus `=` characters are never a conflict marker.

All 26 BLOCKER-severity findings in this report were false positives from this one pattern.

**Fix.** Replace the current regex with one that matches the exact 7-char sigils only, anchored at start-of-line, followed by end-of-line or whitespace:

```python
GIT_CONFLICT_RE = re.compile(
    r'^(<{7}|={7}|>{7}|\|{7})(\s|$)',
    re.MULTILINE,
)
```

Ripgrep equivalent:

```
rg -n --pcre2 '^(<{7}|={7}|>{7}|\|{7})(\s|$)' .
```

Crucially:
- Use `={7}` (exactly 7), not `={7,}` (7 or more), so underlines of 8+ `=` don't match.
- The trailing `(\s|$)` requires the sigil to be alone on its line or followed by whitespace — `<<<<<<< HEAD` matches, `<<<<<<<` alone matches, but `<<<<<<<abc` doesn't.

**Verify.**
- Run the tool against a vendored file full of `===` underlines (e.g. any file with `make tags`-style headers). Should return zero conflict-marker findings.
- Create a fixture with a real conflict:
  ```
  <<<<<<< HEAD
  foo
  =======
  bar
  >>>>>>> branch-name
  ```
  Confirm the pattern still fires on all three marker lines.
- Add both cases as regression tests.

---

## Issue 2 — No `exclude_dirs` for vendored / third-party code

**Evidence.** Out of 26 BLOCKER findings, **all 26** were in `external/dr_libs/*.h`. 5 of the 5 MAJOR "Unsafe C/C++ Functions" findings were also in vendored code (`dr_wav.h`, `dr_flac.h`, `dr_mp3.h`, `stb_image.h`). 30 MAJOR "World-Writable Files" findings were likewise in `external/`. These aren't actionable — the project doesn't maintain upstream code.

**Fix.** Add a config option and honour it across every scanner:

```yaml
# ants-audit.yaml (or whatever the config file name is)
exclude_dirs:
  - external/
  - third_party/
  - vendor/
  - node_modules/
  - .git/
  - build/
```

Default set should exclude those six whether or not the user supplies a config file. Every `find`, `grep` / `rg`, `cppcheck`, and `clang-tidy` invocation must prune these paths:

- `find . -type d \( -name external -o -name third_party -o -name vendor -o -name node_modules -o -name .git -o -name build \) -prune -o -print`
- `rg --glob '!external/**' --glob '!third_party/**' …`
- `cppcheck -isystem external/ …` — treats as system headers; cppcheck skips analysis.
- `clang-tidy` — respect `--exclude-header-filter` or equivalent.

Also make sure per-finding path filtering in any reporting/dedup step honours `exclude_dirs` — there's no point filtering at the scanner level if the reporter re-globs the whole tree.

**Verify.** Re-run the tool on a project with `external/` containing obvious pattern matches (e.g. `strcpy`, `sprintf`, `system()`). BLOCKER and MAJOR counts from `external/` should drop to zero.

---

## Issue 3 — Severity inflation: pattern hits reported at CRITICAL/MAJOR without corroboration

**Evidence.** Current categories include:

- `[CRITICAL] Command Injection Patterns (Security) — 23 findings`
- `[CRITICAL] Hardcoded Secrets Scan (Security) — 13 findings`
- `[CRITICAL] Dynamic Process Spawn — 3 findings`

Each is a single-regex match. A single regex hit is a *candidate*, not a *finding*. Zero of these have been corroborated by a second signal (cppcheck, clang-tidy, known-bad-function list, or a sensitivity-classified path).

When every grep hit is CRITICAL, the consuming Claude session can't distinguish the real CRITICALs from the pattern noise. This is the same signal-drowning problem described in CWE-1120.

**Fix.** Introduce a corroboration layer. Downgrade pure-pattern hits by one severity level (CRITICAL → MAJOR, MAJOR → MINOR) unless one of these is true:

1. **Static-analyser corroboration:** cppcheck or clang-tidy reports an issue at the same `file:line ± 3 lines`.
2. **Path sensitivity:** the match is inside a path listed in `security_sensitive_paths` (configurable; e.g. `auth/`, `crypto/`, `admin/`).
3. **Known-bad-function list:** the match is in a curated list (`strcpy`, `gets`, `system`, `popen`, `eval`, …), not a generic pattern like "the word `exec` appeared somewhere."
4. **User-marked:** a prior run's `.ants-audit-verified.json` (see Issue 5 Phase 2) flagged it as VERIFIED.

Tag corroborated findings explicitly in the output so the consumer can prioritise them:

```
--- [CRITICAL] Command Injection Patterns — 23 finding(s) (2 corroborated) ---
./engine/foo.cpp:142  [CORROBORATED: cppcheck error at line 143]  system("cd " + userPath + " && …")
./engine/bar.cpp:77                                                 exec("sh " + arg)
…
```

Severity promotion rules:
- Uncorroborated CRITICAL → reported as MAJOR.
- Corroborated MAJOR → promoted to CRITICAL.
- Findings in `security_sensitive_paths` retain their original severity but are tagged `[SENSITIVE-PATH]`.

**Verify.** Re-run and confirm:
- cppcheck HIGH-severity findings still appear at their original severity.
- Solo grep hits drop to MAJOR.
- Category headers show `N finding(s) (M corroborated)` so the consumer knows what to triage first.
- The 3 Unsafe-Deserialization and 23 Command-Injection counts don't change, but their severity tags do.

---

## Issue 4 — "World-writable files" on non-POSIX mounts

**Evidence.** 30 findings at `[MAJOR] World-Writable Files`, triggered by a `find -perm -o+w` scan. On filesystems that don't implement Unix permissions (FAT, NTFS, FUSE-mounted remotes), every file shows as world-writable. The user's repo is on such a mount.

**Fix.** Before running the world-writable scan, detect the filesystem of the project root:

```bash
stat -f -c %T .       # Linux — prints e.g. "ext2/ext3", "ntfs", "fuseblk"
stat -f -T .          # macOS — prints e.g. "apfs", "msdos"
```

If the detected filesystem is in `{fat, vfat, exfat, ntfs, ntfs3, fuseblk, fuse, cifs, smbfs, 9p}`, skip the check and emit an informational note instead:

```
--- [INFO] World-Writable Files ---
Scan skipped: project root is on filesystem `ntfs`, which does not enforce
POSIX permissions. All files would appear world-writable regardless of intent.
Run the scan from a POSIX filesystem (ext4, xfs, btrfs, apfs, zfs, …) to
audit permissions.
```

Make the skip list configurable via `skip_worldwrite_on_fs:` so users can add their own.

**Verify.**
- On an ext4 project the check still runs and reports any real world-writable files.
- On an NTFS / FUSE mount the 30-finding bucket collapses to the INFO note.
- Include a test that mocks `stat -f -c %T` and confirms the decision matrix.

---

## Issue 5 — The consumer prompt skips verification, approval, and regression-test gates

**Evidence.** The one-line launch prompt currently in use is:

```
claude "Read /tmp/ants-audit-CgTERZ.txt and fix any real issues found in the
project audit. Focus on bugs, security vulnerabilities, and code quality
problems. Ignore informational items like line counts and file sizes. For each
fix, explain what you changed and why."
```

There's no "verify before fix," no "get approval for non-trivial changes," and no "add a regression test per fix." A Claude session following this prompt will plunge straight into fixing 51 HIGH findings without confirming any of them are real, and without proving the fix works afterward.

**Fix.** Rewrite the prompt to a 5-phase scaffold. Ship it one of two ways (your call):

- **Option A (preferred):** emit it **inside the report header** so the consuming agent reads it first. Can be disabled via `--no-prompt`.
- **Option B:** ship it as a template the user invokes via `ants-audit --prompt` (prints the prompt) or `ants-audit --launch` (runs `claude "$(ants-audit --prompt) Read /tmp/…"`).

**Template:**

```
You are fixing findings from /tmp/ants-audit-<id>.txt. Follow these phases in order.
Do not jump to fixes.

PHASE 1 — BASELINE
Run the existing test suite. Record pass/fail counts, build warnings, lint output.
If anything is broken before you start, surface that first — don't proceed until
the user acknowledges.

PHASE 2 — VERIFY
For every finding tagged BLOCKER, CRITICAL, or MAJOR, mark it VERIFIED,
UNCONFIRMED, or FALSE-POSITIVE. Criteria:
  - Bug:       reproduce with a trace through the code or a failing test.
  - Security:  confirm the pattern is exploitable in context (attacker-reachable
               data flow), not just a regex match on a word.
  - Dead code: confirm no callers — including dynamic dispatch, reflection,
               registries, exported API, tests, and build scripts.
Give each verification a one-line justification with file:line evidence.

PHASE 3 — CITATIONS
For each dependency / CVE finding you intend to fix, cite an authoritative URL
(NVD, GitHub security advisory, upstream project changelog, official docs for
the pinned version). Cross-check the CVE's affected version range against the
pinned version in the lockfile before assuming the CVE applies — a CVE against
a version lower than what's pinned is FALSE-POSITIVE, not a fix target.

PHASE 4 — APPROVAL GATE
Produce a findings list with file:line, severity, verification status, proposed
fix, and blast radius (files touched, public API impact). Wait for user approval
before touching code for any non-trivial finding. Exceptions — you may proceed
directly on:
  - unused imports / obviously dead local helpers with no callers
  - typo'd conditionals that have a reproducing test
  - formatting-only fixes for lint rules already enforced in CI

PHASE 5 — IMPLEMENT + TEST
  - Fix root causes, not symptoms. No --no-verify, no swallowed exceptions,
    no commented-out broken code, no capped iteration counts that hide a real
    divergence. If a workaround is genuinely unavoidable, leave a comment
    documenting the constraint.
  - Every behavioural fix gets a regression test. A fix without a test will
    come back.
  - Keep edits scoped to each finding. No drive-by refactoring of surrounding
    code — drive-bys hide the real fix in review.
  - After fixes, re-run the full suite and include a pre/post diff of:
        tests passed / failed / skipped
        build warnings
        finding counts by severity

DELIVERABLE
At the end, report:
  (1) Findings list with VERIFIED / UNCONFIRMED / FALSE-POSITIVE tags
  (2) Changes made — files touched, why, test evidence
  (3) Deferred items — why deferred, what would unblock them
  (4) Baseline comparison — tests / warnings / findings before vs after
  (5) Allowlist additions — any URLs added during research

Be terse and concrete. Skip categories that had no findings rather than writing
"none found" filler for each.
```

**Verify.**
- Confirm the prompt template appears at the top of `/tmp/ants-audit-*.txt` (or in `ants-audit --prompt` output).
- Dry-run a downstream agent against a small fixture project and confirm its output includes the VERIFIED / UNCONFIRMED / FALSE-POSITIVE tags per finding and a pre/post baseline block.

---

## Nice-to-haves (secondary — ship these after the five above)

- **Line-count / TODO / README tiers → INFO only**, never MAJOR/CRITICAL. The current `[INFO]` bucket seems correct; just ensure nothing in line-count or TODO scanning can leak into higher severities via shared severity tables.
- **Large-files-in-git-history**: dedupe against `.gitignore` and LFS-tracked paths before reporting, so a project that already handles large files with LFS doesn't get flagged.
- **Confidence score semantics**: the `[conf N · user @ date]` annotations in the current output (e.g. `[conf 70 · milnet01 @ 2026-04-08]`) are opaque. Either document what the number means (heuristic score? git-blame-derived confidence?) in the report header, or drop it if it's unused downstream.
- **Don't mix git state with project findings**: the sections `=== Unmerged ===` and `=== Merged (can delete) ===` are branch inventory, not findings. Move them to a separate `Git State` section so they don't crowd the severity counts.
- **Dependency-CVE tier, if absent**: if there isn't one already, add an NVD query tier scoped to versions extracted from the project's lockfile (not bare dep names). Report each CVE with `affects_pinned: true / false / unknown` so the agent can fast-track FALSE-POSITIVE marking in Phase 3.

---

## Acceptance checklist

After your changes, a fresh `ants-audit` run on a C++ project with a vendored `external/dr_libs/` and hosted on an NTFS mount should:

- [ ] Produce **0** `Git Conflict Markers` findings (no false positives from ASCII underlines).
- [ ] Produce **0** findings inside `external/` (and other default-excluded dirs) by default.
- [ ] Produce **0** `World-Writable Files` findings on NTFS / FUSE mounts (replaced by the INFO note).
- [ ] Label corroborated findings explicitly in category headers (e.g. `23 finding(s) (2 corroborated)`); solo grep hits that aren't corroborated are reported at MAJOR maximum.
- [ ] Include the 5-phase prompt template at the top of the report (or provide it via `--prompt`).
- [ ] Preserve existing report format's confidence / blame annotations for downstream tools that already parse them.

## Ship with

- **Tests:**
  - Unit tests for the new `GIT_CONFLICT_RE` pattern against both false-positive (dr_libs-style ASCII) and true-positive (real merge markers) fixtures.
  - `exclude_dirs` plumbing test — a fixture with files in `external/`, `vendor/`, and `src/` confirming only `src/` is scanned.
  - Corroboration-layer test — pattern hit corroborated by mocked cppcheck output is promoted; solo pattern hit is demoted.
  - Filesystem detection test — mocked `stat -f -c %T` returning `ntfs` / `ext4` / `fuseblk` / `apfs` confirms the skip decision.
  - Prompt emission test — `ants-audit --prompt` output contains the 5 phase headers and the deliverable block.
- **CHANGELOG entry** under v0.7.0 (or whatever the next minor version is) grouping these as `Fixed` (Issues 1, 4), `Changed` (Issue 2 — default excludes; Issue 3 — severity semantics), `Added` (Issue 5 — prompt template).
- **Docs update** to the tool's README covering the new `exclude_dirs`, `security_sensitive_paths`, `skip_worldwrite_on_fs`, and `--prompt` options.

---

## Priority order

1. **Issue 1** (conflict-marker regex) — highest signal-to-effort: one regex change, eliminates ~90% of this run's BLOCKERs.
2. **Issue 2** (exclude_dirs) — removes vendored-code noise across every category.
3. **Issue 5** (consumer prompt) — biggest downstream impact; costs you a template file plus a CLI flag.
4. **Issue 3** (corroboration layer) — most work, but highest signal-to-noise improvement on MAJOR/CRITICAL findings.
5. **Issue 4** (NTFS / FUSE skip) — cosmetic but eliminates 30 permanent-false-positive findings for affected users.
