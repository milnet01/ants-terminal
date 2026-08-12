<!-- ants-commit-standards: 2 -->
# Commit Standards — Ants Terminal deltas

> **The standard itself is `~/.claude/standards/commits.md`. Read it there,
> and `~/.claude/standards/releases.md` for cutting a version.** This file
> carries only what is specific to *this* project.

**Why this file is a delta (2026-08-12).** It was a verbatim `/start-app`
copy, last touched 2026-04-30, and it had drifted into instructing
**`git push --tags`** — which global § 4.3 forbids because it publishes every
local tag, including ones never meant to leave the machine. It also shipped a
copy-paste `Co-Authored-By:` block naming a superseded model, which is
exactly the failure global § 1.5 warns about.

## Where the rules actually live

| You want | Read |
|---|---|
| The `<ID>: <description>` mandate and its exceptions | global `commits.md` § 1.1–1.2 |
| Subject constraints, body format, trailers | global `commits.md` § 1.3–1.5 |
| Commit hygiene — one concern, don't amend, don't skip hooks, stage by name | global `commits.md` § 2 |
| Branching and force-push policy | global `commits.md` § 3 |
| Push cadence (public vs private) | global `CLAUDE.md` § 6 |
| **Run the pipeline locally before any push** | global `commits.md` § 4.2 |
| Tags — annotated only, `--follow-tags`, never `--tags` | global `commits.md` § 4.3 |
| Confirm before destructive operations | global `commits.md` § 4.4 |
| Cutting a release | global `releases.md` |
| Anti-patterns | global `commits.md` § 5 |

**Two the drifted copy got wrong, restated so nobody re-derives them from a
stale memory:** push a tag batch with `git push --follow-tags origin main`,
**never** `git push --tags`; and name the model that actually did the work in
`Co-Authored-By:`, at the version currently running — never copied from an
older commit.

## Project-local rules

### This repo is PUBLIC — push freely

`gh repo view --json visibility` reports `PUBLIC`, so CI minutes are free and
global § 4.1's batch-and-ask cadence does not apply. Commit and push each
logical commit. That does **not** exempt you from global § 4.2 — see the
pre-push gate below, which is this project's answer to it.

### The local pipeline gate

Global § 4.2 requires running the pipeline locally before a push and requires
the local run to *execute* the workflow rather than mirror it. This project's
answer:

- **`tools/ci-parity.sh --full`** is the complete mirror — all three `ci.yml`
  jobs, in isolated `build-ci-parity*/` trees. A gate whose tool is absent
  SKIPs loudly and is listed as incomplete parity; it never reports silently
  green.
- **`tools/hooks/pre-push`** runs the reduced form automatically on every
  push (wired via `core.hooksPath=tools/hooks`). It runs the Release suite
  against the warm `build/` **without building**, plus the sanitizer suite
  when a warm ASan tree exists, plus the Qt 6.2 floor guard when the push
  touches compilable source.
- Escape hatches: `git push --no-verify`, `ANTS_PREPUSH_NO_ASAN=1`,
  `ANTS_PREPUSH_NO_QT62=1`.

**Run `--full` before a release, when touching packaging- or e2e-sensitive
code, and whenever a push ADDS a source file.** That last trigger is not
decorative: a new translation unit is the change most likely to reach for a
Qt API newer than the 6.2 floor, and it cost three consecutive red CI runs on
ANTS-4108.

**One push in flight at a time.** Two concurrent pre-push hooks build
`build-asan` simultaneously and produce `mold: unknown file type`, which
looks like corruption and is not.

### Releases are `packaging/cut-rc.sh`, not the global `/release` skill

The weekly Wednesday cadence cuts a public release plus a Patron-preview RC.
The `-rcN` suffix lives **only** at the git tag, GitHub-release title and
AppImage filename — never in `CMakeLists.txt` or `bump.json`. Orchestration
is `packaging/cut-rc.sh` (`new-rc` / `respin` / `promote` / `status` /
`cycle` / `hotfix`); the version bump between phases is `/bump`, which the
script never does itself.

### Version bumps

`project(... VERSION X.Y.Z)` in `CMakeLists.txt` is the single source of
truth. Use `/bump` — its `.claude/bump.json` recipe covers the packaging
carriers — and `packaging/check-version-drift.sh` verifies the lockstep.

## What checks this

`tools/hooks/pre-push` (automatic, every push),
`packaging/check-version-drift.sh` (via `ci-parity.sh --lints` and CI), and
`ci.yml` itself. Nothing checks commit-message *format* — this project has no
`commit-msg` hook and no `.githooks/` directory, so § 1's mandate is read,
not enforced.
