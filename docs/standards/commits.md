<!-- ants-commit-standards: 3 -->
# Commit Standards — Ants Terminal deltas

> **The standard itself is `~/.claude/standards/commits.md`; read
> `~/.claude/standards/releases.md` for cutting a version.** This half of the
> file carries only what is specific to *this* project.
>
> **The owner is mirrored verbatim below the divider**, between the
> `MIRROR BEGIN` / `MIRROR END` markers, because this repo is public and an
> outside reader cannot open a path inside a private home directory. **Do not
> edit that half.** A correction goes upstream, then
> `tools/check-standard-mirrors.sh --write` re-copies it down;
> `tools/hooks/pre-commit` refuses a commit whose mirror has drifted from its
> owner (ANTS-4133). Note that `releases.md` is **not** mirrored — only the
> four deltas' own owners plus `security.md` are.

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
`commit-msg` hook, so § 1's mandate is read, not enforced.
`tools/hooks/pre-commit` does exist, but it checks one thing only: that the
mirrored half of each standard still matches its owner.

**That mirrored half is checked** by `tools/check-standard-mirrors.sh`, which
the pre-commit hook runs; it fails the commit if the text between the MIRROR
markers no longer matches `~/.claude/standards/commits.md`. It skips on a
checkout with no global standards tree — an outside contributor's, or CI's —
since there is then nothing to compare against.

## Review loop log

| Loop | Date | Lanes | Q1/Q2/Q3/Q4 | Outcome |
|---|---|---|---|---|
| 1 | 2026-08-12 | 1 (cold, general-purpose) | Q1 3 · Q2 0 · Q3 1 · Q4 n/a | 4 verified, 4 fixed, 0 dismissed. Attributed `--follow-tags` and the every-local-tag rationale to global `commits.md` § 4.3, which carries neither (they are `CLAUDE.md` § 6) while dropping § 4.3's never-force-push rule; claimed the pre-push hook runs on **every** push when a docs-only push skips it entirely — including a push of this file; claimed all three ci-parity jobs run in `build-ci-parity*/` trees when qt62-baseline runs in a podman volume; gave no route to § 4.2's docs-only exemption conditions. |

---

<!-- MIRROR BEGIN ~/.claude/standards/commits.md -->
# Commit Standards — v1

**Purpose: so that the history explains itself — why each change was
made, and what it belongs to — to someone reading it later without
being able to ask.**

Every rule here traces to that sentence. Git history is the one record
that cannot be rewritten after the fact without cost, so what goes in it
at commit time is all there will ever be.

Governs every commit, plus release-orchestration work under `Kind:
chore` or `release`. See the [index](README.md) for the full set of
standards.


## 1. Commit message format

### 1.1 The `<ID>: <description>` mandate

Every commit subject leads with the ROADMAP item ID it implements,
followed by `:` and a present-tense description:

```
PROJ-1234: implement live-search filter
PROJ-1235: fix config-reload inotify loop
PROJ-1236: extract storeIfChanged helper
```

This connects the commit to the work item end-to-end. A reader of
`git log --oneline` can map every commit back to the ROADMAP entry
that justified it; a reader of the ROADMAP can grep `git log` for
an ID and see exactly which commits implemented it.

The ID prefix replaces the type-based prefix (`feat:`, `fix:`,
`refactor:`) of conventional-commits style — the **kind** is
declared by the ROADMAP item's `Kind:` field, not the commit
subject. This avoids the awkward `PROJ-1234: feat: …` double
prefix.

**This mandate assumes the repo has a ROADMAP.** Where none does, there
is no ID to lead with and §1.2's last row replaces it.

### 1.2 Exception — commits without a ROADMAP item

A few commit types don't ship a ROADMAP-tracked work item; they
use a category prefix instead:

| Type | Format | Example |
|------|--------|---------|
| Release | `X.Y.Z: theme — short summary` | `0.7.55: VT parser correctness + audit-dialog hardening` |
| Chore (debt sweep, gitignore tweak, dep bump) | `chore: short summary` | `chore: post-0.7.55 debt sweep` |
| Doc-only (typo, README tweak not tracked on roadmap) | `docs: short summary` | `docs: fix typo in PLUGINS.md OSC 8 section` |
| Hotfix whose ROADMAP bullet cannot be written yet — the ID is allocated, the bullet back-filled | `fix: short summary` + `Refs: PROJ-NNNN` trailer | see §1.5 |
| **Repo with no roadmap at all** | `<component>: short summary` | `foundation: adopt workflow.md and skeleton/` |

**The last row was added 2026-08-10, and a hook found the gap.** A
config or tooling repo can have no `ROADMAP.md` and no ID prefix, so
§1.1's mandate cannot apply to it — and the four rows above force every
commit into `chore:` or `docs:`, which discards the one thing a reader
of `git log --oneline` wants: *which part of the repo changed*. The
component name carries that. Measured on `~/.claude` itself: twelve
consecutive commits used this form before it was permitted, because it
is what the standard's own §1.1 rationale asks for once you remove the
roadmap.

**`<component>` is a directory or an artefact in the repo**, lowercase,
matching what the change touched — `foundation`, `standards`,
`check-doc-facts`. Not a type (`feat`, `refactor`); §1.1's reasoning
against type prefixes applies here unchanged. It is a name, not a path:
`skeleton`, never `skeleton/files`.

A change that genuinely spans two parts names both, joined by ` + ` —
`standards + skeleton: the two smallest What-checks-this tables`.
Measured on `~/.claude`: five commits needed it. More than two parts is
usually a commit holding more than one concern (§2.1).

The release, chore, docs and fix forms stay available in such a repo,
and the two do not compete: a change that touches an identifiable part
takes that part's name, a repo-wide sweep or a release stays on its
category prefix.

In a repo that has a roadmap: if the work was substantive enough to
be tracked on it (any feature, any non-trivial fix, any refactor), it
gets a ROADMAP item with an ID *first*, then the commit references
that ID. Don't ship code that should have been planned.

git's own generated subjects — `Merge …`, `Revert "…"`, `fixup!` and
`squash!` — are kept verbatim and take no prefix.

### 1.3 Subject line constraints

- Single line, present tense, ≤ 72 chars.
- No trailing period.
- Capitalisation matches the prefix's own case — `PROJ-1234:`,
  `standards:`; the description starts lowercase unless it begins
  with a proper noun.
- Don't repeat the ID in the description ("PROJ-1234: PROJ-1234
  implement live search").

### 1.4 Body

Optional, but encouraged when the change isn't self-explanatory.
Format:

```
PROJ-1234: implement live-search filter

Optional one-paragraph description of the why.

- Bulleted list of specific changes.
- Bulleted list of files / subsystems touched.
- Note any follow-up needed.

Refs: PROJ-1235  (for related but separate work)
Co-Authored-By: <name> <email>
```

Wrap at 72 columns, except a token that cannot be broken — a URL, a
pasted log line. Use the body to explain WHY; the diff shows WHAT.

**A release commit body and a release tag body are exempt.** Both are
the changelog section for that version, copied verbatim
([releases.md](releases.md) §2–3), and changelog bullets routinely run
past 72. Re-wrapping makes the tag stop matching the published notes,
which is the one disagreement that standard exists to prevent — and a
manual re-wrap is a step someone skips, so two people cutting the same
release produce two different tags. Added 2026-08-14 (ROADMAP
CFG-0098).

### 1.5 Trailers

| Trailer | When |
|---------|------|
| `Co-Authored-By:` | Anyone who contributed materially (humans, AI agents) |
| `Reviewed-by:` | After a `/code-quality-review` pass |
| `Fixes:` | When the commit closes a tracker issue (Fixes: #42) |
| `Refs:` | Cross-references — a related ROADMAP item (`Refs: PROJ-1235`), or the back-filled ID of a hotfix (§1.2). The ID is allocated when the commit is made; only the ROADMAP bullet is written later |
| `Signed-off-by:` | DCO-required projects |

For AI-assisted commits, credit the agent in `Co-Authored-By:` as you
would a person — **naming the model that actually did the work**, at
whatever version it currently is. The point is that a future reader can
tell which changes were machine-written and by what; a model name copied
from an older commit defeats that, and models are superseded often
enough that copying is the likely failure.


## 2. Commit hygiene

### 2.1 One concern per commit

If a single commit touches three unrelated subsystems, split it.
The git log is read by the next contributor — make their life
easier.

Exception: cross-cutting refactors (rename, signature change)
that genuinely span the codebase. Note the cross-cutting nature
in the body. The commit ID is the cross-cutting ROADMAP item, where the
repo has one (§1.2).

### 2.2 Always create new commits, don't amend

When a pre-commit hook fails, the commit DID NOT happen — so
`--amend` would modify the *previous* commit, not the failed one.
Fix the issue, re-stage, create a new commit.

Only amend when fixing your *own* unpublished commit before push,
and only if you're certain.

### 2.3 Don't skip hooks

`--no-verify`, `--no-gpg-sign`, etc. bypass project safety nets.
Use only when the user explicitly authorises it for a specific
commit. If a hook fails, investigate and fix the underlying issue
(per [coding § 1.2](coding.md) — no workarounds).

A hook whose own failure message prescribes a bypass for a named
case authorises that case; say why in the commit body. Every other
bypass still needs the user.

### 2.4 Commit only files you mean to

`git add -A` and `git add .` are convenient and dangerous — they
pick up `.env`, `credentials.json`, `node_modules/`,
secret-bearing dotfiles. Add files by name, or use `git add -p`
for staged review.

### 2.5 Don't commit half-finished work

If the commit doesn't build or test green, it doesn't go in. Use
`git stash` for in-progress state. The TDD cycle (per
[testing § 1](testing.md)) means each commit ends with green
tests as a matter of course.

### 2.6 Don't commit generated files

Build artifacts (`build/`, `dist/`, `*.o`, `node_modules/`,
`__pycache__/`) belong in `.gitignore`. Generated docs (`/_build/`,
`docs/_static/`) too. Check `git status` before staging.


## 3. Branching

### 3.1 Trunk-based default

`main` is the integration branch. Short-lived feature branches
fork from `main`, ship via PR (or direct push for solo
development), get rebased + merged in days, not weeks.

### 3.2 Branch names

`<author>/<id>-<topic>` for personal branches: `alice/PROJ-1234-live-search`.
`feature/<id>-<topic>` for shared work. The ID lets a reviewer
find the ROADMAP context at a glance. Where the repo has no roadmap
(§1.2), the component name takes the `<id>` slot.

### 3.3 Don't force-push to shared branches

`git push --force` overwrites remote history. On personal
branches, fine. On `main` / `master` / shared branches, never —
use `git revert` + new commit instead.


## 4. Push policy

### 4.1 Pushing can cost something; committing never does

**A local commit is always free. A push may not be** — it can start CI
that draws on a metered budget. That cost, and nothing about the change
itself, sets the cadence.

**Where a push costs something — a private repository whose pushes burn
CI minutes — stack commits and push the batch.** Every push spends
somebody's quota, so the person paying decides when it is spent, not the
session. Track what is queued and offer the batch; do not push
unprompted.

**Where a push costs nothing — a public repository with free runners, or
any repository with no CI at all — either cadence is fine.** Push per
commit or build up and push together, whichever suits the work. There is
nothing to conserve, so there is no rule to follow.

Two consequences worth stating, because both have been got wrong:

- **Commit locally regardless.** The cost is in pushing. Withholding
  commits to avoid a push loses the history for no saving at all.
- **Establish which case a repository is in once**, at the start of a
  session, and remember it. Guessing per push means guessing wrong
  eventually, and the expensive direction is the silent one.

**A release push is exempt from the batch, on every repository.** Once
a version is bumped, committed and tagged, the release is half-cut
until it is pushed and its checks have run — and a half-cut release is
worse than the minutes it saves: the tag exists on one machine, nothing
is published, and the next session cannot tell a queued release from a
failed one. So [releases.md](releases.md) §6 step 7 pushes without
asking, and this section's batch rule does not apply to it. Everything
else on a metered repository still queues. Written down 2026-08-14
(ROADMAP CFG-0098) because the two standards had said opposite things
and a release completed or hung depending on which was in hand.

Urgency overrides cadence — a security fix, or a push the user asks for,
goes now regardless. The specific per-repository answers for this
machine are in the user's global `CLAUDE.md`; this section owns the
reasoning, that one owns the current answer — and where it sets a
stricter cadence than cost alone would, it wins.

### 4.2 Run the pipeline locally before pushing

**If the repository has a CI pipeline, run it locally first.** A failure
found on your machine costs seconds; the same failure found by CI costs
a push, a wait, a red notification, a fix commit and a second push — and
on a metered repository it costs the minutes twice.

**The local run must execute the pipeline's own definition, not a copy
of it.** This is the whole rule, and it is the part that gets skipped. A
hand-written `ci-local.sh` that mirrors the workflow is correct on the
day it is written and drifts from then on, silently — and a drifted
mirror is *worse than no mirror*, because it returns green for a
pipeline that will fail. You then push with more confidence than if you
had never run it.

So the local runner reads `.github/workflows/*.yml` and executes what it
finds there. [`act`](https://github.com/nektos/act) does exactly this,
running the real workflow in a container. Same principle the rest of
this foundation runs on: **read the source of truth, never re-encode
it.**

**Where a job genuinely cannot run locally** — a self-hosted runner, a
secret that is not on your machine, a platform you do not have — cover
what you can and **write down which jobs are not covered**, next to the
command that runs the rest. An uncovered job nobody has named is
indistinguishable from a covered one, which is the same false
confidence in a different place.

**A repository with a pipeline and no way to run it locally has a gap
worth fixing** before the next feature, not a rule to argue with.

**The docs-only exemption, and the condition that makes it safe.** A
push that changes only documentation may skip the local run **if both
hold**:

1. The last push's CI run on the remote succeeded —
   `gh run list --limit 1 --json conclusion,headBranch`.
2. **No job in the pipeline acts on the paths you changed.** Check the
   workflow rather than assuming.

**The second condition is the one that matters, and it is why this is
not simply "docs are safe".** Plenty of pipelines lint markdown, check
links, spell-check, or build a docs site — and against one of those, a
docs-only change is exactly as capable of going red as a code change.
Taking the exemption without checking is how the rule produces the
failure it exists to prevent. If any job touches your paths, the change
is not docs-only for this purpose, whatever it looks like in the diff.

### 4.3 Tags

Annotated, never lightweight — a lightweight tag carries no message and
no tagger. Push them explicitly rather than relying on a bulk push, and
**never force-push one**: a tag someone has already fetched will not
update for them, so the name comes to mean two different commits. On a
collision, stop and ask.

Release tagging is [releases.md](releases.md) §4, which states the
release tag's own push rule rather than deferring it back here.

### 4.4 Confirm before destructive operations

`reset --hard`, `branch -D`, `clean -f`, `push --force` — pause and
confirm with the user, unless the user has explicitly authorised the
specific operation in advance. §3.3 owns the force-push cases and
overrides this list for them: on a shared branch it is refused rather
than confirmed, on a personal branch it needs neither.

A user approving an action once does NOT approve it in all
contexts.


## 5. Anti-patterns

- ❌ Subject without a ROADMAP ID for substantive work, where the
  repo has a roadmap (§1.2).
- ❌ Subject over 72 characters.
- ❌ "Update files" / "Various changes" / "WIP" as the only
  description.
- ❌ Bundle 5 unrelated changes into one commit because "they
  were all in the working tree".
- ❌ `git commit --amend` after a failed pre-commit hook.
- ❌ `git add -A` / `git add .` instead of naming the paths you
  changed.
- ❌ Force-pushing to a shared branch.
- ❌ Skipping hooks (`--no-verify`) without explicit authorisation.
- ❌ Committing build artifacts / `.env` / credentials.
- ❌ ROADMAP IDs that don't actually exist (typos in the prefix
  or an ID that was never assigned).

Cutting a version — bumping, changelog, tagging, publishing — is
[releases.md](releases.md).

## What checks this

Written 2026-08-10. The first version of this table read *almost
nothing in this standard is checked*, and named the absent `commit-msg`
hook as the largest and cheapest gap. **The hook was written the same
day and the top rows now name it** — which is the table working as
intended: an honest `nothing` is a to-do list, and this is the first row
in the set to have been closed by one.

What remains uncovered is what a hook cannot see. Every rule about the
*diff* rather than the *message* — one concern per commit, no
half-finished work, only the files you meant — is still a judgement, and
the rows below say so.

| Rule | What catches a breach |
|------|----------------------|
| §1.1–1.3 subject shape — the prefix forms, ≤72 characters, no trailing period, no ID repeated in the description | The `commit-msg` hook (`skeleton/files/.githooks/commit-msg`, installed with `git config core.hooksPath .githooks`). Roadmap-aware: it accepts an ID, `X.Y.Z:` or a category prefix in any repo, and *additionally* accepts `<component>: ` where no `ROADMAP.md` and no `.roadmap-counter` exists |
| §1.3 present tense, and the description's capitalisation | **nothing** — both are judgements about wording rather than shape, and a hook cannot make them |
| §9.0 of `documentation.md` — the mechanical checks ran while writing | The `pre-commit` hook (`skeleton/files/.githooks/pre-commit`). **What it blocks on is [documentation.md](documentation.md) §9.0's to state, not this table's** — that section owns the hook's path class and gives it in both directions. A restatement lived here until 2026-08-14 and had already drifted from it (ROADMAP CFG-0098) |
| §1.2 `<component>` names a directory or artefact that exists | **nothing** — the hook matches the component's shape, never that it is real |
| §1.1 and §5 — the ID names a ROADMAP item that exists | **nothing** — the hook matches the ID's shape, never that it was assigned |
| §1.4 body wrapped at 72 columns | **nothing** — the hook checks the subject only. Wrapping is decidable and was left out deliberately: a pasted log line or a URL legitimately exceeds it, and a hook that cries wolf gets bypassed with `--no-verify`, which §2.3 has no check for either |
| §1.5 trailers, and naming the model that did the work | **nothing** — and the failure is silent: a trailer copied from an older commit names a superseded model and reads as correct |
| §2.1 one concern per commit | **nothing** — a judgement about the diff's contents, not its shape |
| §2.2 don't amend | **nothing** for the case §2.2 leads with — an amend after a failed hook is local and leaves no trace. Once a commit is published, `git push` rejects the non-fast-forward |
| §2.3 don't skip hooks | **nothing** — `--no-verify` leaves no trace in the commit |
| §2.4 commit only files you mean to | Partial: a well-maintained `.gitignore` catches the common cases. An allowlist-style ignore file catches more and creates the opposite failure — a file silently never committed (`draft/README.md` records seven lost that way) |
| §2.5 don't commit half-finished work | CI, where the project has it — and `commits.md` §4.2's local run catches it earlier and cheaper |
| §2.6 don't commit generated files | The same `.gitignore`, when it has patterns for that project's build outputs. **Nothing** catches a generated file the ignore file does not name |
| §3.1–3.2 trunk-based default, branch naming | **nothing** — branch names are never validated, and nothing reads the branching shape |
| §3.3 don't force-push a shared branch | Branch protection, **where the plan allows it**. Checked 2026-08-10: unavailable on a private repo without GitHub Pro, so on this machine's private repos it is **nothing** |
| §4.1 push cadence on a metered repo | **nothing** — nothing counts queued commits or asks before spending quota |
| §4.2 the pipeline runs locally before a push | **nothing** on the rule itself. CI going red catches the *failure*, which is precisely the cost the rule exists to avoid — a check that fires after the damage is not a check on the rule |
| §4.3 annotated tags, never lightweight | **nothing** — and a lightweight tag is invisible until someone reads its absent message |
| §4.4 confirm before a destructive operation | **nothing** — by construction: the confirmation is the check, and nothing checks the confirmation happened |

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|------|------|-------|----|----|----|----|---------|
| 1 | 2026-08-11 | 3 | 2 | 4 | 2 | 0 | 8 findings, 6 verified / 2 dismissed. All 6 fixed; 1 cross-doc item surfaced (`CLAUDE.md` §6 tag push) and 1 code-side question (the hook's `a + b: ` form). Loop 2 dispatched. |
| 2 | 2026-08-11 | 3 | 2 | 4 | 2 | 0 | 8 findings, 7 verified / 1 dismissed. All 7 fixed; 2 of them were loop 1's own repairs. Loop 3 dispatched. |
| 3 | 2026-08-11 | 3 | 1 | 5 | 2 | 0 | 10 findings, 8 verified / 2 dismissed. All 8 fixed; 2 were loop 2's own repairs. **Cap reached, not converged** — see the tail in `docs/reviews/commits-md-review-2026-08-11.md`. |
<!-- MIRROR END -->
