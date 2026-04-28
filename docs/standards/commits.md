<!-- ants-commit-standards: 1 -->
# Commit Standards — v1

A shareable contract for git commits in this project. Pairs with
the other three standards in this folder ([coding](coding.md),
[documentation](documentation.md), [testing](testing.md)) — see
the [index](README.md) for the full set.

This standard governs every commit, plus the release-orchestration
work under ROADMAP bullets with `Kind: chore` or `release`.

---

## 1. Commit message format

### 1.1 The `<ID>: <description>` mandate

Every commit subject leads with the ROADMAP item ID it implements,
followed by `:` and a present-tense description:

```
ANTS-1234: implement live-search filter
ANTS-1235: fix config-reload inotify loop
ANTS-1236: extract storeIfChanged helper
```

This connects the commit to the work item end-to-end. A reader of
`git log --oneline` can map every commit back to the ROADMAP entry
that justified it; a reader of the ROADMAP can grep `git log` for
an ID and see exactly which commits implemented it.

The ID prefix replaces the type-based prefix (`feat:`, `fix:`,
`refactor:`) of conventional-commits style — the **kind** is
declared by the ROADMAP item's `Kind:` field, not the commit
subject. This avoids the awkward `ANTS-1234: feat: …` double
prefix.

### 1.2 Exception — commits without a ROADMAP item

A few commit types don't ship a ROADMAP-tracked work item; they
use a category prefix instead:

| Type | Format | Example |
|------|--------|---------|
| Release | `X.Y.Z: theme — short summary` | `0.7.55: VT parser correctness + audit-dialog hardening` |
| Chore (debt sweep, gitignore tweak, dep bump) | `chore: short summary` | `chore: post-0.7.55 debt sweep` |
| Doc-only (typo, README tweak not tracked on roadmap) | `docs: short summary` | `docs: fix typo in PLUGINS.md OSC 8 section` |
| Hotfix without prior ROADMAP entry (will be back-filled) | `fix: short summary` + `Refs: ANTS-NNNN` trailer | see §1.4 |

If the work was substantive enough to be tracked on the roadmap
(any feature, any non-trivial fix, any refactor), it gets a
ROADMAP item with an ID *first*, then the commit references that
ID. Don't ship code that should have been planned.

### 1.3 Subject line constraints

- Single line, present tense, ≤ 72 chars.
- No trailing period.
- Capitalisation matches the ID's case (`ANTS-1234:`); the
  description starts lowercase unless it begins with a proper
  noun.
- Don't repeat the ID in the description ("ANTS-1234: ANTS-1234
  implement live search").

### 1.4 Body

Optional, but encouraged when the change isn't self-explanatory.
Format:

```
ANTS-1234: implement live-search filter

Optional one-paragraph description of the why.

- Bulleted list of specific changes.
- Bulleted list of files / subsystems touched.
- Note any follow-up needed.

Refs: ANTS-1235  (for related but separate work)
Co-Authored-By: <name> <email>
```

Wrap at 72 columns. Use the body to explain WHY; the diff shows
WHAT.

### 1.5 Trailers

| Trailer | When |
|---------|------|
| `Co-Authored-By:` | Anyone who contributed materially (humans, AI agents) |
| `Reviewed-by:` | After a `/indie-review` pass |
| `Fixes:` | When the commit closes a tracker issue (Fixes: #42) |
| `Refs:` | Cross-references — e.g. `Refs: ANTS-1235` for related ROADMAP items |
| `Signed-off-by:` | DCO-required projects |

For AI-assisted commits, include the AI's identifier:

```
Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
```

---

## 2. Commit hygiene

### 2.1 One concern per commit

If a single commit touches three unrelated subsystems, split it.
The git log is read by the next contributor — make their life
easier.

Exception: cross-cutting refactors (rename, signature change)
that genuinely span the codebase. Note the cross-cutting nature
in the body. The commit ID is the cross-cutting ROADMAP item.

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

---

## 3. Branching

### 3.1 Trunk-based default

`main` is the integration branch. Short-lived feature branches
fork from `main`, ship via PR (or direct push for solo
development), get rebased + merged in days, not weeks.

### 3.2 Branch names

`<author>/<id>-<topic>` for personal branches: `alice/ANTS-1234-live-search`.
`feature/<id>-<topic>` for shared work. The ID lets a reviewer
find the ROADMAP context at a glance.

### 3.3 Don't force-push to shared branches

`git push --force` overwrites remote history. On personal
branches, fine. On `main` / `master` / shared branches, never —
use `git revert` + new commit instead.

---

## 4. Push policy

### 4.1 Public repos — push freely

Public GitHub repos get unlimited Linux-runner minutes for free
(per GitHub Actions billing). Push after each release without
batching.

### 4.2 Private repos — batch + ask

Private repos consume CI minutes against the account quota.
Default behaviour: commit locally, don't push without explicit
user approval. Track unpushed commits / tags; surface a prompt
when the queue grows (≥ 5 commits or tags).

When push time comes, send the whole batch in one go
(`git push origin <branch> && git push --tags`). One CI run for
the branch + N concurrent release-workflow runs is the most
minute-efficient way to ship a batch.

Counter-cases (push immediately even on a private repo): critical
security fix on a shipped binary; user explicitly says "push this
one"; CI signal needed before merging a feature branch.

### 4.3 Tag format

Annotated tags only:

```bash
git tag -a vX.Y.Z -m "X.Y.Z"
```

Don't create lightweight tags (`git tag vX.Y.Z`) for releases —
they don't carry the release message.

Push tags explicitly: `git push origin vX.Y.Z` for one,
`git push --tags` for a batch. **Don't force-push tags under any
circumstance** — if a tag collision happens, stop and ask.

### 4.4 Confirm before destructive operations

`reset --hard`, `branch -D`, `clean -f`, `push --force` to a
shared branch — pause and confirm with the user, unless the user
has explicitly authorised the specific operation in advance.

A user approving an action once does NOT approve it in all
contexts.

---

## 5. Release commits (`Kind: release`)

Release commits use the `X.Y.Z: theme — summary` format from
§1.2 plus a categorical body drawn from the CHANGELOG entry:

```
0.7.55: VT parser correctness + audit-dialog hardening

Tier-1 fixes:

- ANTS-1042: HIGH — ESC-in-OSC dispatches RIS as side-effect.
- ANTS-1043: HIGH — Audit-pipeline rule-quality file pollutes git status.

Tier-2 hardening:

- ...

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
```

Note that the *bullets* inside the release body still cite
ROADMAP IDs — the release commit aggregates many ID-tracked
items into one shipping point.

Touched files for a release commit are typically: every
version-bearing file (`CMakeLists.txt`, packaging files,
`README.md`, `ROADMAP.md`), `CHANGELOG.md` (new dated section),
and the implementation changes themselves.

If a `.claude/bump.json` recipe exists, the `/bump` skill handles
the version-bearing-file edits. Otherwise, do it manually and run
any project-specific drift checks (e.g.
`packaging/check-version-drift.sh`) before committing.

---

## 6. Releases on public hosts

After a release commit + tag on a public GitHub repo:

```bash
gh release create vX.Y.Z \
    --title "X.Y.Z — <theme>" \
    --notes "$(extract-changelog-section X.Y.Z)"
```

The notes body is the corresponding `[X.Y.Z]` section from
`CHANGELOG.md`. Use a heredoc to preserve markdown formatting.

CI (if wired up) will fire on the tag push and attach build
artifacts (AppImage, MSI, .dmg) to the release automatically.
Don't manually upload artifacts that CI will produce.

---

## 7. Anti-patterns

- ❌ Subject without a ROADMAP ID for substantive work.
- ❌ Subject that doesn't fit on one screen.
- ❌ "Update files" / "Various changes" / "WIP" as the only
  description.
- ❌ Bundle 5 unrelated changes into one commit because "they
  were all in the working tree".
- ❌ `git commit --amend` after a failed pre-commit hook.
- ❌ `git add .` with no review.
- ❌ Force-pushing to a shared branch.
- ❌ Skipping hooks (`--no-verify`) without explicit authorisation.
- ❌ Committing build artifacts / `.env` / credentials.
- ❌ Lightweight tags for releases (`git tag vX.Y.Z` without
  `-a`).
- ❌ Pushing a release tag whose CI hasn't run / passed.
- ❌ Force-pushing tags.
- ❌ ROADMAP IDs that don't actually exist (typos in the prefix
  or an ID that was never assigned).
