<!-- ants-commit-standards: 2 -->
# Commit Standards — Ants Terminal deltas

> **The standard itself is `~/.claude/standards/commits.md`. Read it there,
> and `~/.claude/standards/releases.md` for cutting a version.** This file
> carries only what is specific to *this* project.

**Why this file is a delta (2026-08-12).** It was a verbatim `/start-app`
copy, last touched 2026-04-30, and it had drifted into instructing
**`git push --tags`** — which global `commits.md` § 4.3 forbids as a bulk tag
push, and which global `CLAUDE.md` § 6 rules out by name because it publishes
every local tag, including ones never meant to leave the machine. It also
shipped a copy-paste `Co-Authored-By:` block naming a superseded model, which
is exactly the failure global § 1.5 warns about.

## Where the rules actually live

| You want | Read |
|---|---|
| The `<ID>: <description>` mandate and its exceptions | global `commits.md` § 1.1–1.2 |
| Subject constraints, body format, trailers | global `commits.md` § 1.3–1.5 |
| Commit hygiene — one concern, don't amend, don't skip hooks, stage by name | global `commits.md` § 2 |
| Branching and force-push policy | global `commits.md` § 3 |
| Push cadence (public vs private) | global `CLAUDE.md` § 6 |
| **Run the pipeline locally before any push** | global `commits.md` § 4.2 |
| Tags — annotated only, pushed explicitly, never force-pushed (stop and ask on a collision) | global `commits.md` § 4.3 |
| The tag-batch command: `--follow-tags`, never `--tags` | global `CLAUDE.md` § 6 |
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
  jobs. `build-test` and `build-asan` run in isolated `build-ci-parity*/`
  trees; `qt62-baseline` runs in a podman container against a cached image and
  build volume (needs podman; `tools/qt62-guard.sh --clean` reclaims them). A
  gate whose tool is absent SKIPs loudly and is listed as incomplete parity;
  it never reports silently green.
- **`tools/hooks/pre-push`** runs the reduced form automatically (wired via
  `core.hooksPath=tools/hooks`). It runs the Release suite against the warm
  `build/` **without building**, plus the sanitizer suite when a warm ASan
  tree exists, plus the Qt 6.2 floor guard when the push touches compilable
  source.
- **A docs-only push skips the hook entirely** — it mirrors `ci.yml`'s
  `paths-ignore`, so a push touching nothing outside `ROADMAP.md`,
  `CHANGELOG.md`, `README.md`, `PLUGINS.md`, `LICENSE`, `.roadmap-counter` and
  `docs/` runs no gate at all. Editing *this file* and pushing it runs
  nothing. Global § 4.2's exemption then applies and **its two conditions are
  yours to check by hand**: the last remote run was green, and no job acts on
  the changed paths. The second is not vacuous here — `ci.yml`'s
  `pull_request` trigger has no `paths-ignore`, so the same change opened as a
  PR does get the lint gate.
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

`tools/hooks/pre-push` (automatic on every push that touches something outside
the docs-only set above — a docs-only push is gated by nothing),
`packaging/check-version-drift.sh` (via `ci-parity.sh --lints` and CI), and
`ci.yml` itself. Nothing checks commit-message *format* — this project has no
`commit-msg` hook and no `.githooks/` directory, so § 1's mandate is read,
not enforced.

## Review loop log

| Loop | Date | Lanes | Q1/Q2/Q3/Q4 | Outcome |
|---|---|---|---|---|
| 1 | 2026-08-12 | 1 (cold, general-purpose) | Q1 3 · Q2 0 · Q3 1 · Q4 n/a | 4 verified, 4 fixed, 0 dismissed. Attributed `--follow-tags` and the every-local-tag rationale to global `commits.md` § 4.3, which carries neither (they are `CLAUDE.md` § 6) while dropping § 4.3's never-force-push rule; claimed the pre-push hook runs on **every** push when a docs-only push skips it entirely — including a push of this file; claimed all three ci-parity jobs run in `build-ci-parity*/` trees when qt62-baseline runs in a podman volume; gave no route to § 4.2's docs-only exemption conditions. |
