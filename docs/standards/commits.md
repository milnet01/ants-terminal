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

**This mandate assumes the repo has a roadmap.** Where none does, there
is no ID to lead with and §1.2's last row replaces it — and that row
owns the test for which repos those are.

### 1.2 Exception — commits without a ROADMAP item

A few commit types don't ship a ROADMAP-tracked work item; they
use a category prefix instead:

| Type | Format | Example |
|------|--------|---------|
| Release | `X.Y.Z: theme — short summary` | `0.7.55: VT parser correctness + audit-dialog hardening` |
| Chore (debt sweep, gitignore tweak, dep bump) | `chore: short summary` | `chore: post-0.7.55 debt sweep` |
| Doc-only (typo, README tweak not tracked on roadmap) | `docs: short summary` | `docs: fix typo in PLUGINS.md OSC 8 section` |
| Hotfix whose ROADMAP bullet cannot be written yet — the ID is allocated, the bullet back-filled | `fix: short summary` + `Refs: PROJ-NNNN` trailer | see §1.5 |
| **Repo with no roadmap at all** — neither `ROADMAP.md` nor `.roadmap-counter` | `<component>: short summary` | `foundation: adopt workflow.md and skeleton/` |

**The last row was added 2026-08-10, and a hook found the gap.** A
config or tooling repo can have neither file, and so no ID prefix, so
§1.1's mandate cannot apply to it. The four rows above then force every
commit into `chore:` or `docs:`, which discards the one thing a reader
of `git log --oneline` wants: *which part of the repo changed*. The
component name carries that. Measured on `~/.claude` itself: twelve
consecutive commits used this form before it was permitted, because it
is what the standard's own §1.1 rationale asks for once you remove the
roadmap.

**Both files decide it, and §1.1, §3.2 and the `commit-msg` hook key on
that one test.** A repo carrying a `.roadmap-counter` and no
`ROADMAP.md` has a roadmap for this purpose, and `<component>:` is
refused there.

**`<component>` is a directory or an artefact in the repo**, lowercase,
matching what the change touched — `foundation`, `standards`,
`check-doc-facts`. Not a type (`feat`, `refactor`); §1.1's reasoning
against type prefixes applies here unchanged. It is a name, not a path:
`skeleton`, never `skeleton/files`.

A change that genuinely spans two parts names both, joined by ` + ` —
`standards + skeleton: the two smallest What-checks-this tables`.
Measured on `~/.claude`: five commits needed it. More than two parts is
usually a commit holding more than one concern (§2.1).

**Where it genuinely is §2.1's cross-cutting exception, name the two
largest parts and say so in the body.** The ` + ` list never runs past
two — a longer one stops being readable in `git log --oneline`, which is
the whole reason the component name is there. §2.1 sends this case here
for repos with no roadmap, and without this sentence it went back and
forth between the two sections.

The release, chore, docs and fix forms stay available in such a repo,
and the two do not compete: **a change one of those rows names keeps its
category prefix, and everything else takes the part's name.** So a
gitignore tweak or a dependency bump is still `chore:` and a typo fix is
still `docs:`, as their rows show.

**`fix:` comes without its trailer there.** Its row pairs the form with
a `Refs: PROJ-NNNN` naming an allocated ID, and a repo with no roadmap
has none — §5 makes inventing one an anti-pattern. So the form stays and
the trailer does not apply. A hotfix touching an identifiable part still
reads better as that part's name.

In a repo that has a roadmap: if the work was substantive enough to
be tracked on it (any feature, any non-trivial fix, any refactor), it
gets a ROADMAP item with an ID *first*, then the commit references
that ID. Don't ship code that should have been planned.

git's own generated subjects — `Merge …`, `Revert "…"`, `fixup!` and
`squash!` — are kept verbatim. **That means no prefix and no §1.3 limit:
neither the 72 characters nor the trailing period applies**, and §5's
anti-pattern list reads under this exemption. Shortening a reverted
subject to fit would break the link to what it reverts, which is the
whole value of keeping it. The `commit-msg` hook exits before every
§1.3 check for these four forms.

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
the changelog section for that version, copied verbatim.
[releases.md](releases.md) §§2–3 says that of the commit body. **The tag
half is this section's own rule, by decision** (ROADMAP CFG-0098 item 9),
so do not read the citation as covering it. §2 there now enumerates the
tag body too, added 2026-08-18 (ROADMAP CFG-0143) so this citation has a
source rather than resting on the sentence above; the wrap exemption
itself stays here, which is what CFG-0098 item 9 decided.

Changelog bullets routinely run past 72, and re-wrapping makes the tag
stop matching the published notes — the one disagreement that standard
exists to prevent. A manual re-wrap is also a step someone skips, so two
people cutting the same release produce two different tags. Added
2026-08-14 (ROADMAP CFG-0098).

### 1.5 Trailers

| Trailer | When |
|---------|------|
| `Co-Authored-By:` | Anyone who contributed materially (humans, AI agents) |
| `Reviewed-by:` | After a `review-code` pass |
| `Fixes:` | When the commit closes a tracker issue (Fixes: #42) |
| `Refs:` | Cross-references — a related ROADMAP item (`Refs: PROJ-1235`), or the back-filled ID of a hotfix (§1.2). The ID is allocated when the commit is made; only the ROADMAP bullet is written later, and **before the change is pushed** — after that the history cites a bullet no reader can open. Nothing checks it |
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
case authorises that case. Every other bypass still needs the user.

**A named case names the CIRCUMSTANCE, not the command.** *"No network —
skip with X"* authorises that push; a blanket *"fix it, or bypass with
X"* printed on every failure authorises nothing, because it names no case
to be in. Without this, a gate that prints its own escape hatch
self-authorises every bypass of itself, which empties this section for
exactly the gates that have one. So **a bypass of a check that actually
FAILED always needs the user** — that failure is the thing the check
exists to report.

**Every bypass says why in the commit body, whichever it was.**
[coding § 1.2](coding.md) requires the reason on every workaround and
names `--no-verify` in its list, so a user-authorised skip owes one too.
The body note is the only trace either leaves.

**Scope: a bypass at COMMIT time. A push-time bypass has no commit body
to be written in**, because every commit already exists by then and §2.2
forbids amending one to add the note. Skipping the §4.2 gate —
`git push --no-verify`, or the `SKIP_LOCAL_CI=1` form a hook prescribes
— is recorded **in the body of the next commit**, naming what was
skipped and why. Where there is no next commit it is recorded nowhere, and
the *What checks this* table's §2.3 row says so.

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
fork from `main`, get rebased + merged in days, not weeks.

**Whether they ship via PR is the project's to say, not this
section's.** The default is a direct push to `main`; a project opts into
PR-based feature work, and on this machine the user's global `CLAUDE.md`
§7 owns that opt-in and its signals. Same hand-back §4.1 makes for push
cadence.

### 3.2 Branch names

`<author>/<id>-<topic>` for personal branches: `alice/PROJ-1234-live-search`.
`feature/<id>-<topic>` for shared work. The ID lets a reviewer
find the ROADMAP context at a glance. Where the repo has no roadmap
(§1.2), the component name takes the `<id>` slot.

**A two-part component collapses its ` + ` join to `-` here** —
`standards + skeleton` becomes `alice/standards-skeleton-two-tables`. A
git refname cannot contain a space, so the §1.2 spelling is not a legal
branch name and every author would invent a different collapse.

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
failed one. So [releases.md](releases.md) §6 step 8 pushes without
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

**So invert it: put the checks in one script the repository owns, and
have `.github/workflows/ci.yml` set up a machine and call that script.**
Then there is one list of steps, the local run and the remote run cannot
disagree, and the script is a thing you can give a flag to — which the
documentation mode below requires and a container full of YAML cannot
offer. LocalWebServerManager is the worked example; its `ci.yml` header
says outright that the script owns every actual check.

**A repository-owned gate script is only a mirror when the workflow does
not call it.** That distinction is the whole rule, and without it the
paragraph above appears to forbid the arrangement this one prescribes.

**Where the pipeline cannot be inverted** — a workflow you do not
control, a job matrix with no single entry point —
[`act`](https://github.com/nektos/act) runs the real workflow in a
container and is the fallback. Same principle either way: **read the
source of truth, never re-encode it.**

**Run it over the commits you are pushing, not over your working
tree.** Those are the same tree only when the tree is clean, and nothing
makes it clean. An uncommitted fix turns the run green for commits that
will go red. Unrelated scratch work turns it red for commits that were
fine. Both answers are about a tree the remote will never see, and
neither announces itself.

A pre-push hook is where this bites. git hands the hook the exact range
on stdin — one `<local ref> <local sha> <remote ref> <remote sha>` line
per ref — and a hook that then runs the gate from the repo root has
thrown it away. **The same range decides the docs-only exemption
below**, so a hook that reads it for one and not the other is already
half right.

**Route 1 — check the pushed commits out somewhere else and run the
gate there.**

```bash
tree=$(mktemp -d)
trap 'git worktree remove --force "$tree" 2>/dev/null' EXIT
git worktree add --detach --quiet "$tree" "$local_sha"
( cd "$tree" && ./scripts/local-ci.sh )
```

That runs against what the remote will see, and it never touches the
working tree. The cost is a checkout with none of your ignored files in
it, so a gate that needs a virtualenv, a build tree or a downloaded
fixture pays to make one on every push.

**Route 2 — where that cost is too high, refuse a dirty tree instead**
— exit non-zero when `git status --porcelain` is non-empty, and name the
files.
It is a worse experience and an honest one: the developer is told the
run cannot answer for this push, rather than shown a verdict that does
not.

**A clean tree is not enough on its own, and this is the condition that
gets left out.** It proves the tree matches `HEAD` — never that `HEAD`
is what you are pushing. `git push origin other-branch` from `main`,
`git push origin HEAD~2:main`, and a push carrying several refs all pass
the porcelain check while the gate answers for a commit the remote will
never receive. So route 2 is available **only when every pushed tip
equals `HEAD`**: compare each `<local sha>` on stdin against
`git rev-parse HEAD`, and where any differs, refuse or take route 1.

**Never stash to manufacture a clean tree.** `git stash` exits 0 having
stashed nothing when there is nothing to stash, so the hook cannot tell
a clean tree from a failed stash — [`languages/cpp.md`](languages/cpp.md)
§ Tests records the same trap producing a false pass. A stash also
mutates the developer's tree mid-hook, and the `pop` has to survive a
gate that may have just exited non-zero, on a machine where the push was
interrupted.

**Where a job genuinely cannot run locally** — a self-hosted runner, a
secret that is not on your machine, a platform you do not have — cover
what you can and **write down which jobs are not covered**, next to the
command that runs the rest. An uncovered job nobody has named is
indistinguishable from a covered one, which is the same false
confidence in a different place.

**A repository with a pipeline and no way to run it locally has a gap
worth fixing** before the next feature, not a rule to argue with.

**A docs-only push runs the documentation checks — not the whole
pipeline, and not nothing.** Both extremes are wrong in the same way. A
full run charges ninety seconds of engine tests for a typo fix, and that
is how a person learns to reach for `--no-verify` by reflex; a blanket
skip walks past the markdown lint, the link check, the docs build and
the test that counts something in a README. Measured on this machine
(OneUp, 2026-08-21): the documentation mode of that project's gate runs
in 0.14s where its full gate takes about 90s, and it still covers every
check a `.md` edit can reach.

So **give the gate a documentation mode and select it by the paths in
the push**. Where the gate has no such mode, run all of it and say so —
never quietly select a subset the gate does not offer. A hook cannot
invent a check, and a skipped check followed by a green line is
indistinguishable from a clean run.

**Which files count as documentation is a per-project answer, and
getting it wrong is not symmetric.** A file the pipeline READS is a
pipeline input whatever its extension: a test that asserts against
`README.md`, a check that counts headings, a docs site that builds from
`docs/`. On 2026-08-19 a LocalWebServerManager push edited `CLAUDE.md`,
took the exemption on the strength of the `.md`, and GitHub found the
prose count that project's own suite forbids. So decide by what the
pipeline reads, never by the extension, and let every uncertain case run
the full gate — a wrong guess must cost time, never coverage.

**A generic hook cannot do this on its own, so it must be told.** One
shared between repositories has no way to know what a given pipeline
reads, and its default will therefore be a list of extensions — which is
the thing this paragraph forbids. `~/.claude/githooks/pre-push` takes the
answer from `git config ants.gate.docsGlob`, per repository. **A
repository whose pipeline reads markdown must narrow that glob to exclude
those paths, or keep its own hook.** Nothing checks that it did.

**The remaining case, where there is nothing to run at all.** Where the
pipeline has no job that acts on documentation, the documentation checks
are the empty set and the push may skip the local run entirely — **if
both hold**:

**This skip outranks the run-all-of-it fallback above.** Both fire on the
same push — a docs-only push, to a repository whose gate has no
documentation mode and whose pipeline has no documentation job — and they
prescribe opposite things. A gate with no documentation mode is not a
reason to run it when there is provably nothing in it for your paths.
Condition 2 is what proves that, which is why it is the condition that
matters.

1. The last push's CI run on the remote succeeded —
   `gh run list --branch <the branch being pushed> --event push
   --limit 1 --json conclusion`. **Take the branch from the ref being
   pushed, never from `git branch --show-current`** — that reads the
   working tree, which this section has just finished saying is not what
   is being pushed, so a hook run for `git push origin feature` from
   `main` would check `main`'s last conclusion and grant the skip on it.
   The `<remote ref>` field of the hook's own stdin line names it —
   **with the prefix stripped**, `${remote_ref#refs/heads/}`. git supplies
   that field fully qualified and `--branch` wants a bare branch name.
   Measured 2026-08-21: `--branch main` returned two runs on a repository
   where `--branch refs/heads/main` returned `[]`, so a hook substituting
   the field directly gets an empty list on every push and condition 1
   can never hold — a skip that silently never fires, indistinguishable
   from one denied on the merits. **Both filters are load-bearing.** `gh
   run list` scopes by neither branch nor event, so `--limit 1` alone
   returns the most recent run of any kind on any branch — a green
   scheduled run while the last push went red is exactly the case this
   condition excludes.
2. **No job in the pipeline acts on the paths you changed.** Check the
   workflow rather than assuming.

**The second condition is the one that matters, and it is why this is
not simply "docs are safe".** Plenty of pipelines lint markdown, check
links, spell-check, or build a docs site — and against one of those, a
docs-only change is exactly as capable of going red as a code change.
Taking the exemption without checking is how the rule produces the
failure it exists to prevent. If any job touches your paths, this skip
is not available — run those jobs, which is the documentation mode
above, whatever the diff looks like.

### 4.3 Tags

Annotated, never lightweight — a lightweight tag carries no message and
no tagger. Push them explicitly rather than relying on a bulk push, and
**never force-push one**: a tag someone has already fetched will not
update for them, so the name comes to mean two different commits. On a
collision, stop and ask.

**`git push --follow-tags` is the explicit form; `git push --tags` is
the bulk push this rules out.** `--follow-tags` sends only the annotated
tags reachable from the commits being pushed, so the branch you named
decides what leaves the machine. `--tags` sends every local tag,
including ones never meant to leave it. Pushing the tag by name in a
second command is explicit too. Where the project's workflows trigger on
tags it buys a second CI run for nothing, and for a release tag
[releases.md](releases.md) §4 rules out **the separate tag push** by
name: that section prescribes `--follow-tags` and requires the tag to go
"with the commit it names, not separately and not later". Settled 2026-08-18 (ROADMAP CFG-0133): this
section said "explicitly" and named no command, while `releases.md` §4
and the global `CLAUDE.md` §6 both prescribe `--follow-tags`. A
conformer running the prescribed command could not tell whether it
breached this rule.

Release tagging is [releases.md](releases.md) §4, which states the
release tag's own push rule rather than deferring it back here.

### 4.4 Confirm before destructive operations

`reset --hard`, `branch -D`, `clean -f`, `push --force` — pause and
confirm with the user, unless the user has explicitly authorised the
specific operation in advance. §3.3 owns the branch force-push cases and
overrides this list for them: on a shared branch it is refused rather
than confirmed, on a personal branch it needs neither. §4.3 owns the tag
case: a force-push is refused there, in advance or at the time. On a
collision §4.3 sends you to the user to pick a different tag or to
abandon the release — forcing it is not on the menu.

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
| §1.1–1.3 subject shape — the prefix forms, ≤72 characters, no trailing period, no ID repeated in the description | The `commit-msg` hook (`.githooks/commit-msg`, enabled via `core.hooksPath`; `skeleton/files/` ships an identical copy). Roadmap-aware: it accepts an ID, `X.Y.Z:` or a category prefix in any repo, and *additionally* accepts `<component>: ` where no `ROADMAP.md` and no `.roadmap-counter` exists |
| §1.3 single line | **nothing**, and it defeats the length check too. The hook reads `head -1`; git's subject is the whole first paragraph, joined. Measured: a 70-character line 1 with a second line under it passes the hook and produces a 134-character subject in `git log --oneline` |
| §1.3 present tense, and the description's capitalisation | **nothing** — both are judgements about wording rather than shape, and a hook cannot make them |
| §9.0 of `documentation.md` — the mechanical checks ran while writing | Partial: the `pre-commit` hook (`.githooks/pre-commit`, enabled via `core.hooksPath`). **Passing it is not §9.0 satisfied** — that section says so, and names quoted fragments and census counts as not checked at all. `check-doc-facts` runs the rest. **What it blocks on is [documentation.md](documentation.md) §9.0's to state, not this table's** — that section owns the class list, and says which classes the weaker copy in `skeleton/files/` does not carry. A restatement lived here until 2026-08-14 and had already drifted from it (ROADMAP CFG-0098) |
| §1.2 `<component>` names a directory or artefact that exists | **nothing** — the hook matches the component's shape, never that it is real |
| §1.1 and §5 — the ID names a ROADMAP item that exists | **nothing** — the hook matches the ID's shape, never that it was assigned |
| §1.4 body wrapped at 72 columns | **nothing** — the hook checks the subject only. Wrapping is decidable and was left out deliberately: a pasted log line or a URL legitimately exceeds it, and a hook that cries wolf gets bypassed with `--no-verify`, which §2.3 has no check for either |
| §1.5 trailers, and naming the model that did the work | **nothing** — and the failure is silent: a trailer copied from an older commit names a superseded model and reads as correct |
| §2.1 one concern per commit | **nothing** — a judgement about the diff's contents, not its shape |
| §2.2 don't amend | **nothing** for the case §2.2 leads with — an amend after a failed hook is local and leaves no trace. On a published commit it depends on the branch: on a shared one `git push` rejects the non-fast-forward and §3.3 refuses the force-push, but on a **personal** branch §3.3 permits it and §4.4 asks for no confirmation, so there it is **nothing** as well — and that is where most amending happens |
| §2.3 don't skip hooks | **nothing mechanical** — `--no-verify` leaves no trace in the commit itself, so §2.3's required body note is the only one there is, and nothing checks that it was written. A **push**-time bypass (`--no-verify`, or a hook's own `SKIP_LOCAL_CI=1` form) is worse: it belongs in the next commit's body, and where there is no next commit it is recorded **nowhere at all** |
| §2.4 commit only files you mean to | Partial: a well-maintained `.gitignore` catches the common cases. An allowlist-style ignore file catches more and creates the opposite failure — a file silently never committed (`draft/README.md` records seven lost that way) |
| §2.5 don't commit half-finished work | CI, where the project has it — and `commits.md` §4.2's local run catches it earlier and cheaper |
| §2.6 don't commit generated files | The same `.gitignore`, when it has patterns for that project's build outputs. **Nothing** catches a generated file the ignore file does not name |
| §3.1–3.2 trunk-based default, branch naming | **nothing** — branch names are never validated, and nothing reads the branching shape |
| §3.3 don't force-push a shared branch | Branch protection, **where the plan allows it**. Checked 2026-08-10: unavailable on a private repo without GitHub Pro, so on this machine's private repos it is **nothing** |
| §4.1 push cadence on a metered repo | **nothing** — nothing counts queued commits or asks before spending quota |
| §4.2 the pipeline runs locally before a push | A `pre-push` hook, where one is installed **and reached**. It refuses the push, so it catches the breach rather than the failure. Three ways it is not reached, all silent. `core.hooksPath` holds **one** value and the repository-local one wins, so a project setting it for its own `commit-msg` never runs `~/.claude/githooks/pre-push` and must copy a `pre-push` into its own hooks directory — which a skeleton-scaffolded project does not ship. Nothing checks that `core.hooksPath` was set at all; it is per-clone and cannot be committed. And the machine-wide hook **exits 0 when it discovers no gate script**, so a repository with a pipeline and no local runner — §4.2's "gap worth fixing" — pushes green forever with the hook installed |
| §4.2 the local run is over the pushed commits, not the working tree | `~/.claude/githooks/pre-push` and LocalWebServerManager's hook take route 1 unconditionally, so where either runs the rule cannot be breached. Everywhere else **nothing**, and this one is invisible from both sides — a gate run over a dirty tree returns an ordinary verdict with no sign that it answered for a tree nobody is pushing. Checked 2026-08-21: no project has a test asserting its hook takes either route |
| §4.3 annotated tags, never lightweight | **nothing** — and a lightweight tag is invisible until someone reads its absent message |
| §4.4 confirm before a destructive operation | **nothing** — by construction: the confirmation is the check, and nothing checks the confirmation happened |

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|------|------|-------|----|----|----|----|---------|
| 1 | 2026-08-11 | 3 | 2 | 4 | 2 | 0 | 8 findings, 6 verified / 2 dismissed. All 6 fixed; 1 cross-doc item surfaced (`CLAUDE.md` §6 tag push) and 1 code-side question (the hook's `a + b: ` form). Loop 2 dispatched. |
| 2 | 2026-08-11 | 3 | 2 | 4 | 2 | 0 | 8 findings, 7 verified / 1 dismissed. All 7 fixed; 2 of them were loop 1's own repairs. Loop 3 dispatched. |
| 3 | 2026-08-11 | 3 | 1 | 5 | 2 | 0 | 10 findings, 8 verified / 2 dismissed. All 8 fixed; 2 were loop 2's own repairs. **Cap reached, not converged** — see the tail in `docs/reviews/commits-md-review-2026-08-11.md`. |
| 4 | 2026-08-18 | 3, cold — genre pinned `standard`, first loop of a NEW run, gating the CFG-0133 edit. Packet carried both hooks in full, `releases.md` §§4 and 6, `documentation.md` §9.0, `CLAUDE.md` § Git push, and `git push --help`'s `--follow-tags` text verbatim | 2 | 4 | 0 | n/a | **Six verified, six fixed; none dismissed. Two more found by the orchestrator's own sweep.** **All three lanes independently found two defects.** The first is the sharpest: the What-checks-this table named `skeleton/files/.githooks/pre-commit` as what enforces `documentation.md` §9.0 — and that copy has two classes where the installed hook has seven. §9.0 says so itself ("**`path` is absent too** … the skeleton ships the weaker `link` class in its place"), so a scaffolded project was told its commits were checked for something nothing checks. Both hook rows now name the installed `.githooks/` path. The second: §4.4 carved force-pushes out to §3.3, which covers branches only — so a tag force-push fell back to §4.4's *confirm, unless pre-authorised*, where §4.3 refuses it outright. Two lanes found §1.2's roadmap test keying on `ROADMAP.md` alone while the hook sets `has_roadmap=yes` on `.roadmap-counter` too; a repo mid-setup was refused a form the standard granted it. **One lane alone found the run's quietest defect**: §1.2 exempts `Merge`/`Revert`/`fixup!`/`squash!` subjects from the *prefix* rule and says nothing about §1.3, while the hook exits before the length and trailing-period checks as well — measured here, a 102-character `Revert "…"` with a trailing period passes and the same length without the prefix fails. And §3.1 mandated PR-based feature work where `CLAUDE.md` §7 makes direct-to-`main` the default; §3.1 now hands that answer back the way §4.1 hands back cadence. **Both orchestrator findings came from settling a lane's open question rather than from a lane**: `gh run list` is not branch-scoped, so §4.2 condition 1's `--limit 1` returned the most recent run on *any* branch and answered a different question than the condition asks; and fixing the pre-commit row left its `commit-msg` sibling still naming the skeleton tree. **One dismissed as code-side and filed**: the hook counts subject length with `wc -m`, which returns characters under the user's UTF-8 locale and bytes under `LC_ALL=C` — measured 29 vs 31 on the standard's own em-dash release example. |
| 5 | 2026-08-18 | 3, cold — identical brief, packet rebuilt from disk and extended with the INSTALLED `.githooks/pre-commit` class header, which loop 4's fixes now point at | 0 | 6 | 4 | n/a | **Ten verified, ten fixed; none dismissed. Not one Q1 from a lane** — every lane defect was two passages disagreeing or a conformer with no way to tell. **Three of the ten landed on loop 4's own text, and two of those were mine rather than a lane's.** The sharpest lane finding is one loop 4 half-fixed: the §9.0 row's file path was corrected and its *coverage* claim was left standing, where `documentation.md` §9.0 says outright **"Passing the hook is not this rule satisfied"** and names quoted fragments and census counts as unchecked — so a green hook read as §9.0 discharged. The row is now `Partial:`. **The best pre-existing finding needed no cross-reference at all**: §1.2 offered the `fix:` form to a repo with no roadmap, and that form is *defined* by a `Refs: PROJ-NNNN` trailer naming an allocated id — which such a repo cannot allocate, while §5 makes inventing one an anti-pattern. A conformer hot-fixing a config repo had no conforming option. **Two rows of the What-checks-this table were overstating their catcher.** §2.2's said `git push` rejects a published amend; §3.3 permits the force-push on a personal branch and §4.4 asks for no confirmation, which is where most amending happens. And §1.3's *single line* was covered by no row — **measured here, a 70-character first line with a second line under it passes the hook and yields a 134-character subject**, because the hook reads `head -1` and git's subject is the whole first paragraph joined. **That measurement refuted my own first draft of the fix**, which said `git log --oneline` truncates it; it does not, it joins, and the defect is worse than the row I nearly shipped. Also fixed: §1.2's prose reserved category prefixes for *a repo-wide sweep or a release* while its own table's chore and docs examples name single artefacts; §3.2 had no legal spelling for §1.2's two-part ` + ` component, a git refname having no space (`git check-ref-format` refuses it); §1.5's back-fill deadline was unbounded, now the push; and §2.3 required a written reason for the hook-prescribed bypass only, where `coding.md` §1.2 requires one for every workaround and names `--no-verify`. **My own two:** loop 4 asserted a second tag push *"buys a second CI run"* with no `on:` condition, and left §4.4's tag refusal ambiguous about approval given live at a collision. |
| 6 | 2026-08-18 | 3, cold — identical brief, packet rebuilt from disk and extended with the four facts loop 5's lanes could not settle without `Bash` | 2 | 2 | 1 | n/a | **Five verified, five fixed; one dismissed. Cap reached (3 for a standard); the run files its tail and exits.** **A VIOLENT cap, and the run is oscillating: three of the five landed on text THIS RUN wrote** — checked against the earlier loops' ledger rows, not recall. **All three lanes independently found the one substantial pre-existing defect**, and it is one loop 5 DISMISSED as immaterial: § 1.4 attributes the release *tag* body's content to `releases.md` §§ 2–3, and § 2's enumeration names the commit body, the published notes and any announcement — not the tag; § 4 requires it be annotated without saying what it carries. A lane named the build difference loop 5 could not: a release tool written from § 4 tags with `-m "Release 0.7.55"`, one written from § 1.4 pastes the changelog section, and both are conformant. **The self-inflicted three share one shape — each was a fix that solved half of a two-half defect.** Loop 4 added `--branch` to § 4.2's command and not `--event`, so a green *scheduled* run still answered for a red push (`gh run list` scopes by neither). Loop 5 corrected the § 9.0 row's file path and left its coverage claim. And loop 5 resolved the unusable `fix:` form by PROHIBITING it in a roadmap-less repo — where `commit-msg` tests `category_form` before the `has_roadmap` branch and accepts `fix:` everywhere, so the prohibition enforced nothing and no row listed it; the form is restored and only its trailer withdrawn. **The second pre-existing finding was a mutual deferral neither section closed**: § 2.1 sends a cross-cutting refactor to § 1.2 *"where the repo has one"*, and § 1.2 caps its ` + ` guidance at two parts and sends anything wider back to § 2.1 as *"usually"* more than one concern — so a genuine cross-cutting rename in a roadmap-less repo had no prescribed subject and `component_form` accepts every spelling. **One finding was the sweep catching the same loop's own fix**: the § 1.4 repair implied CFG-0143 would relocate the wrap exemption, which CFG-0098 item 9 had already decided belongs here. **Size is not the reason the cap bound** — 456 lines, well inside what two cold reads finish, and the lanes reached § 5 and the closing table every loop. |
| 7 | 2026-08-21 | 3, cold — genre pinned `standard`, first loop of a NEW run, gating the CFG-0182 edit. Packet carried `languages/cpp.md` § Tests, `workflow.md` § 6 as written the same day, `CLAUDE.md` rule 6, the new machine-wide hook in full, and eight measured source facts | 0 | 4 | 3 | n/a | **Seven verified, seven fixed; none dismissed. Not one Q1 from a lane.** **All three lanes independently found the same defect, and it was the change's own foundation**: § 4.2 said the local runner *"reads `.github/workflows/*.yml` and executes what it finds there"* via `act`, while the worked example this edit added runs `./scripts/local-ci.sh` — the repository-owned script the paragraph above calls *"worse than no mirror"*. Two lanes went further and found the half that made it unbuildable: the new docs-only rule says to *"give the gate a documentation mode"*, and a workflow executed through `act` has no flag to give it. `workflow.md` § 6, written the same hour, resolved it the other way and cited *"`commits.md` § 4.2 states the principle"* — a principle § 4.2 did not state. The section now states the inversion outright — one repository-owned gate script that `ci.yml` also calls — with `act` demoted to the fallback, and adds the distinction the whole rule turns on: **a gate script is only a mirror when the workflow does not call it.** **All three lanes also found the same Q3**, and it is this edit's own: route 2 (*refuse a dirty tree*) proves the tree matches `HEAD`, never that `HEAD` is what you are pushing. Reproduced here — a clean tree, `git push origin HEAD~2:main`, and a push made from a different branch all pass `git status --porcelain` while the gate answers for a commit the remote will never receive. Route 2 now requires every pushed tip to equal `HEAD`. **One lane alone found the quietest defect, and it is the same shape in pre-existing text**: condition 1 of the skip reads the branch from `git branch --show-current` — the working tree — eleven lines after the section finishes saying the working tree is not what is being pushed. **Two Q3s about traces that do not exist:** § 2.3 requires every bypass to state its reason in the commit body, and a push-time bypass happens after every commit exists while § 2.2 forbids amending one, so skipping the new gate had nowhere to be recorded; and § 4.3's *"rules it out by name"* had two available antecedents prescribing opposite commands for a release tag — `releases.md` § 4 settles it as the separate tag push. **Two What-checks-this rows were made false by this run's own hook**, both written hours earlier: they read **nothing** while `~/.claude/githooks/pre-push` and LocalWebServerManager's hook take route 1 unconditionally. **The 4b sweep found three more copies of the retired design** — `CLAUDE.md` § 6 and `releases.md` § 6 step 5 both restated the `act` route as § 4.2's requirement, and `releases.md`' What-checks-this row said a release is unchecked when `act` is absent, which is now only one of two routes. **One lane surfaced a code-side finding correctly rather than filing it as a doc defect**: the machine-wide hook decides documentation by extension while this section says never to. Real, and unfixable generically — the hook now says so in those words rather than implying its default is compliant. |
| 8 | 2026-08-21 | 3, cold — identical brief, packet rebuilt from disk and extended with `releases.md` § 4 and § 6 step 5, plus the reproduced clean-tree/wrong-tip cases | 2 | 3 | 4 | n/a | **Nine verified, nine fixed; none dismissed. Six of the nine landed on text loop 7 wrote** — 4a-min's pattern at its starkest, and every one in text a fix ADDED. **The sharpest was settled by measurement rather than reading.** Loop 7 fixed condition 1 to take the branch from the hook's `<remote ref>` field; git supplies that field fully qualified, and `gh run list --branch` wants a bare name. Run here: `--branch main` returned two runs on a repository where `--branch refs/heads/main` returned `[]`. So loop 7's repair produced a skip that can never fire, indistinguishable from one denied on the merits — a worse defect than the one it fixed. One lane filed it as a Q1; a second raised it as the open question that named the one `gh` call which would settle it. **Two lanes found that loop 7 referred to *route 1* and *route 2* in two places and numbered neither**, so a hook author self-assessing against the coverage row had to guess which was which — and guessing wrong builds the porcelain check as the tip≠`HEAD` fallback, which is the exact failure the sentence closes. **All three lanes found the docs-classification rule has no owner.** § 4.2 says to decide by what the pipeline reads and never by the extension; the hook the table endorses decides by extension by default and says so in its own comment. The rule was unbuildable as written — a hook shared between repositories cannot know what a pipeline reads — so the section now names the per-repository knob that tells it, and the table says nothing checks the glob was narrowed. **One lane alone found the run's best pre-existing Q2**, and it empties § 2.3 for exactly the gates that matter: a hook's failure message prescribing a bypass *authorises* it, and the machine-wide hook printed `fix it, or push once with: SKIP_LOCAL_CI=1` on **every** failure — so the one gate with a scripted bypass self-authorised every bypass of itself. A named case now names the CIRCUMSTANCE, not the command, and a bypass of a check that actually failed always needs the user. The hook's message was changed with it. **A second lane-1 Q1 was the coverage row overstating its own hook, twice**: `core.hooksPath` holds one value and the local one wins, so a project setting it for `commit-msg` silently loses the machine-wide `pre-push` — which is every skeleton-scaffolded project, since `skeleton/files/.githooks/` ships no `pre-push`. And the hook exited 0 on discovering no gate script, so a repository with a pipeline and no local runner pushed green forever with the hook installed; it now says which of the two cases it is. **One Q2 was two of loop 7's own rules firing on one push**: *run all of it* and *skip entirely* both apply to a docs-only push to a repo whose gate has no docs mode and whose pipeline has no docs job, and neither yielded. The skip now outranks. **Both code-side findings were surfaced by lanes rather than misfiled as document defects**, which is the brief working. |
<!-- MIRROR END -->
