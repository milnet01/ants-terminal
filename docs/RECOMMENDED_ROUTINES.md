# Recommended Claude Code Routines — Ants Terminal

This document captures the routines that earn their daily-quota slot
(`0/15` on Max, `0/5` on Pro) for this repository. Each routine is
ready to paste into the **New routine** form at
[claude.ai/code/routines](https://claude.ai/code/routines) — or via
`/schedule` in any session for the schedule-only triggers.

Routines run on Anthropic-managed cloud infrastructure as full Claude
Code sessions. They count against your account's daily routine cap
(separate from interactive subscription usage), and anything they push
appears as **you** via your connected GitHub identity. See [Automate
work with routines](https://code.claude.com/docs/en/routines) for the
full mechanics.

---

## Why these specific routines

The Ants Terminal repo already has every "deterministic, repeatable,
unattended" workflow handle a routine could plausibly drive:

- An in-tree `/audit` skill (`auditdialog.cpp` is the in-process
  implementation; equivalent CLI invocation is what each cloud session
  would run).
- The `/security-review` and `/review` skills.
- A version-drift script (`packaging/check-version-drift.sh`).
- A typed `ROADMAP.md` with `📋 Planned` markers grep-able as work
  candidates.
- A `tests/features/` regression-test pattern that's the natural place
  for a routine to file PRs.

The four routines below cover the top recurring asks: **catch bugs
fast** (nightly audit), **review external PRs without waiting on a
human** (PR review), **keep the audit tool itself sharp** (weekly
triage), and **never lose a roadmap item to drift** (monthly roadmap
chooser).

Skip any that don't fit your workflow. The point of a written list is
to make the cost-vs-benefit explicit, not to fill the quota.

---

## 1. Nightly audit triage

**What it does.** Runs the project's audit catalogue end-to-end, diffs
against the last clean baseline, and opens a PR with only the *new*
findings (suppressed/known ones stay out of the way).

**Trigger.** Schedule → **Daily**, 03:00 in your local timezone (so the
PR is waiting when you start work). Stagger keeps it within ~10 min.

**Repository.** `milnet01/ants-terminal` (default branch). Allow
unrestricted branch pushes: **off** — Claude only needs `claude/*`.

**Environment.** Default. Setup script (one-time, cached):

```bash
# Ants needs Qt6 headers + cppcheck/clazy/clang-tidy for the C++ lanes.
sudo apt-get update -y
sudo apt-get install -y qt6-base-dev cppcheck clang-tidy clazy \
    libgl1-mesa-dev libxkbcommon-dev libxcb-xkb-dev pkg-config cmake ninja-build
```

**Connectors.** None required. (Add Slack if you want a posted summary
on PR open; skip otherwise — each connector widens the trust scope of
the run.)

**Paste this prompt into the form:**

```
Run a full project-audit pass on the Ants Terminal repository checked out at the
session root. The audit catalogue lives in src/auditdialog.cpp — for this routine,
reproduce it from the command line by invoking the same external tools the dialog
runs:

1. cd build && cmake .. -GNinja && ninja -j$(nproc)
2. cppcheck --enable=all --std=c++20 --library=qt --suppress=missingIncludeSystem
   --suppress=unusedFunction --suppress=unknownMacro -I src src/ 2>&1 | tee /tmp/cppcheck.txt
3. clazy-standalone -p build --checks=connect-3arg-lambda,lambda-in-connect,
   container-inside-loop,old-style-connect,range-loop-detach,qstring-arg,qgetenv
   src/*.cpp 2>&1 | tee /tmp/clazy.txt
4. The grep-rule lane: replicate each addGrepCheck pattern from auditdialog.cpp.
   Apply OutputFilter (dropIfContains + dropIfContextContains + maxLines) per
   the C++ definition. Skip files matching the kFindExcl / kGrepExcl lists.

Compare findings against the last `audit_baseline.json` if one exists at the
repo root (or against the most recent `docs/AUTOMATED_AUDIT_REPORT_*.md` if
not).

If there are NEW findings (not in the baseline):
- Open a PR titled `audit: triage findings from <YYYY-MM-DD> nightly run`
  on a `claude/audit-triage-<YYYY-MM-DD>` branch.
- Body: a markdown table of new findings (file:line, rule, severity, snippet).
- For each NEW finding, propose a verdict in the body: REAL / FALSE-POSITIVE /
  RULE-TIGHTEN-CANDIDATE. Include reasoning.
- If you assess any finding is a clear-cut FALSE-POSITIVE that warrants a
  rule-tightening (regex, dropIfContains, dropIfContextContains), include the
  exact src/auditdialog.cpp diff in the PR body as a fenced ```diff block.
  DO NOT push the diff to a branch — that's a follow-up human decision.

If there are NO new findings, do not open a PR. Instead, write a one-line
status to `docs/AUDIT_BASELINE_STATUS.md` (create it if missing): the date,
total finding count, and `clean` / `regressed` verdict.

Reference: docs/AUDIT_TRIAGE_2026-04-16.md is the gold-standard triage
format — match its tone (concise, file:line for every claim, cite the rule
and the suppression that should fire if applicable).
```

**Why nightly, not on every commit.** A clean repo doesn't need
Claude's attention; a regression caught within 24 h is fast enough,
and the daily cadence keeps the routine quota usable for other work.

---

## 2. Pull-request security review

**What it does.** Runs `/security-review` against every PR opened
against `main`, leaves inline comments for findings, and a summary
comment so a human reviewer can focus on design.

**Trigger.** GitHub event → `pull_request.opened` on
`milnet01/ants-terminal`. Filters: `base branch equals main`,
`is draft equals false`, `from fork equals false` (don't auto-run on
fork PRs without a manual greenlight; opt-in via a `safe-to-review`
label if you want fork coverage).

**Repository.** `milnet01/ants-terminal`. Branch pushes restricted to
`claude/*` (the routine writes review comments via the GitHub App,
not commits).

**Environment.** Default. Setup script: same as Routine #1 (Qt6 +
cppcheck + clazy). Both routines share the cache.

**Connectors.** None required.

**Paste this prompt into the form:**

```
A pull request has just been opened against milnet01/ants-terminal. The PR
context (title, body, base/head SHAs) is available via `gh pr view ${PR_NUMBER}
--json title,body,baseRefName,headRefName,additions,deletions,files`.

Step 1 — read STANDARDS.md, CONTRIBUTING.md, and CLAUDE.md. These are the
project's gold-standard contracts; every review claim should cite one.

Step 2 — run the equivalent of the in-tree /security-review skill:
- Invoke `gh pr diff ${PR_NUMBER}` to get the patch.
- Categorize the diff: src/* (production), tests/* (test-only),
  packaging/* (release infra), docs/* (text-only).
- Apply the security checks that are appropriate per category. For src/*,
  go deep: input validation, integer overflow, command injection, OSC
  payload bounds, plugin sandbox escapes, signal-slot lifetime traps
  (especially QNetworkReply 3-arg connects per the 0.6.5 incident that
  motivated the qnetworkreply_no_abort audit rule).
- For Qt-specific patterns, run clazy on the changed translation units only:
  clazy-standalone -p build --checks=connect-3arg-lambda,lambda-in-connect,
  container-inside-loop,range-loop-detach <changed-cpp-files>

Step 3 — leave inline comments via `gh pr review --comment` ONLY where there
is a concrete, file:line-anchored finding. Each comment must:
- Quote the offending line.
- State the issue in one sentence.
- Cite the standard or precedent (STANDARDS.md §X, audit rule ID, prior
  incident commit SHA).
- Suggest a concrete fix (or say "hand to maintainer" if scope is unclear).

Step 4 — leave a single summary comment that lists what categories you
checked, what you found (or "no findings"), and what you intentionally
did NOT check (e.g. "skipped GLSL — needs visual inspection on a real GPU,
out of scope for an automated pass").

Do NOT push commits. Do NOT change the PR's review status to APPROVE or
REQUEST CHANGES — leave it as a comment-only review so the human reviewer
makes the verdict call.
```

**Why opt-in for forks.** Routines act as your GitHub identity. A
hostile fork PR could craft a code-execution payload that the routine
runs in its own sandbox; the sandbox is isolated, but better to gate
it behind the `safe-to-review` label maintainers add manually.

---

## 3. Weekly audit-rule self-triage

**What it does.** Looks at the last 7 days of audit runs (whether from
Routine #1, the in-app dialog, or CI), counts how many times each rule
fired with each verdict, and proposes rule tightenings for any rule
whose FP rate exceeds a threshold. This is the **self-learning** loop
the audit catalogue needs to keep its signal-to-noise ratio over time.

**Trigger.** Schedule → **Weekly**, Sunday 18:00 local. (Sunday so the
PR lands before Monday's work; before-the-week, not during.)

**Repository.** `milnet01/ants-terminal`.

**Environment.** Default. No special setup.

**Connectors.** None required.

**Paste this prompt into the form:**

```
Audit-tool self-improvement pass. Goal: keep the audit catalogue's signal-to-
noise ratio high by tightening rules whose FP rate has crept up.

Inputs to read:
- docs/AUTOMATED_AUDIT_REPORT_*.md and trend_snapshot_*.json from the last
  7 days. These are emitted by the in-app audit dialog AND by Routine #1.
- docs/AUDIT_TRIAGE_2026-04-16.md as the canonical example of what a triage
  document looks like.
- src/auditdialog.cpp — current rule definitions (regex, dropIfContains,
  dropIfContextContains, severity).
- .audit_suppress (JSONL) — every finding the user has manually suppressed,
  with reason + timestamp.

Computation:
1. For each rule_id, count: total fires this week, fires-then-suppressed
   (look up dedup keys in .audit_suppress with a timestamp inside the
   week), fires-still-open, fires-already-fixed (dedup key disappeared
   from later runs without entering .audit_suppress).
2. Compute the suppression rate: suppressed / total. A rate >= 50 % means
   the rule is producing more noise than signal and should be tightened.
3. For each rule above the threshold, look at the actual matched lines in
   the report files. Identify the COMMON shape of the false-positive
   matches. Propose a rule tightening: regex change, dropIfContains
   addition, dropIfContextContains addition, severity demotion to Info,
   or rule deletion.

Outputs:
- If at least one rule needs tightening, open a PR titled
  `audit: weekly self-triage <YYYY-MM-DD> — N rules tightened` on a
  `claude/audit-self-triage-<YYYY-MM-DD>` branch with the proposed
  src/auditdialog.cpp diff and a triage table mirroring
  docs/AUDIT_TRIAGE_2026-04-16.md's format.
- ALWAYS write docs/AUDIT_QUALITY_<YYYY-MM-DD>.md with the per-rule stats
  table (rule_id, fires, suppressed, suppression_rate%) — this becomes the
  audit-tool's quality dashboard over time. Even with no tightenings to
  propose, the dashboard is the durable signal.

DO NOT modify .audit_suppress. The user's manual suppressions are sacred
input data, not output.
```

**Why weekly, not nightly.** A week is the smallest window with enough
data to discriminate "noisy rule" from "noisy week." Nightly would
churn rules on small samples.

---

## 4. Monthly ROADMAP item scoper

**What it does.** Picks one `📋 Planned` item from `ROADMAP.md` (or
the next `🚧 In progress` if any), researches the prior art linked in
the item, and writes a design doc PR proposing the implementation
shape. Doesn't write code — just gets the conversation started.

**Trigger.** Schedule → **Weekly**, first Sunday of each month, 12:00
local. (Custom cron via `/schedule update`: `0 12 1-7 * 0`.)

**Repository.** `milnet01/ants-terminal`.

**Environment.** Default with **Network access: full** (the prompt
needs to fetch prior-art docs).

**Connectors.** None required.

**Paste this prompt into the form:**

```
Pick one ROADMAP item to scope. Procedure:

1. Read ROADMAP.md. Find all `📋 Planned` items in the current release
   (the section under `## 0.X.0 — <theme> (target: ...)`). If none in
   the current release, look at the next.
2. Pick the one that most fits these criteria, in order:
   (a) Has an explicit "prior art" link.
   (b) Is in scope for a single self-contained design doc.
   (c) Has not already been picked — check `git log --grep "design:"
       --since=60.days.ago` to filter out items recently scoped.
3. Fetch and read the prior-art links. Extract the relevant API surface,
   data flow, edge cases.
4. Write `docs/<YYYY-MM>_DESIGN_<item-slug>.md` containing:
   - Problem statement (one paragraph, restating the ROADMAP entry in
     plain language).
   - Prior art summary with citations (each claim links a source URL).
   - Proposed approach: data model, key APIs, where it slots into the
     existing architecture (cite specific src/* file ranges).
   - Migration story: does it break any existing API or config? (Always
     answer this question even if the answer is "no").
   - Test strategy: which tests/features/ harness to extend, what new
     test would lock the contract.
   - Open questions for the maintainer to resolve.
5. Open a PR titled `design: <item title> — proposed approach` on a
   `claude/design-<item-slug>` branch with this doc. Body: a one-line
   summary + a request that the maintainer review the design before
   any implementation begins.

Do NOT implement the feature. The point is to start a design conversation
the maintainer can react to, not to deliver code that has to be unwound
if the design is rejected.
```

**Why monthly.** Roadmap design is a slow-burn activity. Monthly gives
enough cadence to keep moving; weekly would queue up faster than a
maintainer can review.

---

## Routine usage budget — the math

Daily Max cap is **15 runs**. Used by these four routines:

| Routine                          | Cadence    | Runs/day average |
|----------------------------------|------------|------------------|
| 1. Nightly audit triage          | Daily      | 1                |
| 2. PR security review            | On PR open | ≤2-3 typical     |
| 3. Weekly audit self-triage      | Weekly     | 0.14             |
| 4. Monthly ROADMAP scoper        | Monthly    | 0.03             |
| **Total budget consumed**        |            | **~3-4 / day**   |

Leaves ~11 daily slots for ad-hoc `/schedule` invocations during
interactive sessions or for adding more routines later (a Vestige PR
review running off the same key would land in this budget too — see
that repo's `docs/RECOMMENDED_ROUTINES.md`).

If you flip to Pro (5/day cap), drop Routine #2 to opt-in via a
`/security-review-please` label or run it locally in CI via the
GitHub Actions integration instead — it's the only one likely to
spike past the cap on a busy PR day.

---

## Maintenance

When the project's audit-rule catalogue, version-drift script, or
roadmap structure changes in a way that breaks one of the prompts
above, **update this file in the same commit as the change**. The
prompts are documentation-as-code: stale prompts mean broken nightly
runs and PR comments that quote the wrong invariants.

The hook in `.claude/settings.json` watches CMakeLists.txt edits, but
not this file — review `RECOMMENDED_ROUTINES.md` whenever you touch
`auditdialog.cpp`, `ROADMAP.md`, or the audit-suppress format.
