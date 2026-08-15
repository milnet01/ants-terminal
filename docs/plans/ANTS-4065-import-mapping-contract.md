# ANTS-4065 — build order: reach a reliable markdown→store import

**Spec:** [`docs/specs/ANTS-4065-import-mapping-contract.md`](../specs/ANTS-4065-import-mapping-contract.md)
(accepted 2026-08-08). The reasoning lives there; this file is the order of
operations and how each step is checked.

**Where we are.** This project is migrated and the store is primary, but the
import that produced it loses declared fields and invents others, and the
markdown it renders no longer round-trips. So the current store is not a
trustworthy base, and the current `ROADMAP.md` is not a trustworthy source —
the render has already written the import's mistakes back into it.

**The ordering rule everything else follows:** *fix the source, then the code,
then re-import.* Re-importing today's file would reproduce today's losses,
because nothing about the file or the parser has changed.

---

## Phase A — Restore a trustworthy source

**A1. Revert the roadmap files to `6d9e743d`.**
`git checkout 6d9e743d -- ROADMAP.md docs/roadmap/0.6.md docs/roadmap/0.5.md`

That commit is the last state before the first store render: it **keeps** the
101 Layman lines and the two malformed-bullet repairs, and **drops** the 180
materialised ids, the 363 rendered `Source: planned.` lines and the 123 rewritten
`Kind` values.

> **Verify** — and note which checks actually discriminate, because the obvious
> one does not:
>
> | Check | Before revert | After revert |
> |---|---|---|
> | `grep -c '^  Source: planned\.'` | 399 | **36** |
> | `grep -oE '\[ANTS-[0-9]+\]' \| sort -u \| wc -l` | 1,888 | **1,705** |
> | `grep -c '^  Layman:'` | 114 | **114** (unchanged — the work survives) |
>
> `git diff --stat` shows the three roadmap files and nothing else.
>
> **Do not use `grep -c 'Kind: bug'` as the check** — it returns 35 at both
> revisions. The render writes `it.body` verbatim and appends the trailer after
> it, so a bullet whose body carries an inline `Kind: bug.` keeps that text *and*
> gains a `Kind: implement.` trailer. Counting the string finds the surviving
> body prose, not the coerced field. (That same duplication is one of the shapes
> feeding the round-trip drift Phase D4 diagnoses.)

> **A1 also drops four bullets that are not render damage** — ANTS-4062, 4063,
> 4064 and 4065 were appended *after* `6d9e743d`, along with the status audit's
> two flips (ANTS-3754, ANTS-3853) and its two body notes. Replay them, taken
> from their own commit diffs rather than retyped. Final id count is 1,709, not
> 1,705. Done 2026-08-08 in `2143afed`.

**A2. Wipe the store.**
`rm ~/.local/share/ants-terminal/roadmap.sqlite` — **and its `-wal` and `-shm`
siblings**, which the first draft of this step omitted. The WAL alone was 5 MB
and would have restored the store on the next open.

Safe and reversible: it is derived, gitignored and machine-local, and Phase D
rebuilds it.

> **A running Ants instance keeps the deleted store alive, and this is the trap
> to plan around.** SQLite holds the inode open, so `rm` unlinks the name and
> nothing more: `ls -l /proc/$(pgrep -x ants-terminal)/fd | grep roadmap`
> showed five fds on `roadmap.sqlite (deleted)` after the wipe. In that state
> `roadmap_query` still answers from the ghost store — it reported ANTS-3853 as
> in-progress while the reverted file said planned — and **any `roadmap_log`
> write re-renders all three files from the ghost, undoing Phase A.**
>
> **So: relaunch Ants before the first MCP write, and do any pre-relaunch
> roadmap edit directly rather than through `roadmap_log`.** The same applies at
> E2 to every project whose store is wiped or rebuilt while Ants is open.

> **Verify (after the relaunch):** no `roadmap.sqlite` fd in
> `/proc/<pid>/fd`; `roadmap_query` agrees with the file; and
> `roadmap_log op:"annotate"` succeeds without a `render_gate_unmet` refusal —
> proving the project is back on the markdown backend.

---

## Phase B — Audit the source, now that it is the source

Order matters within the phase: B1 makes B2's numbers trustworthy.

**B1. Re-run the corpus survey — and fix it first. DONE (2026-08-08).**

The step as first written assumed the survey's figures were wrong only because
it had been run *after* the render. **Tested and false:** re-running it against
the reverted pre-render source still reported no `bug` at all. The actual cause
is that `tools/roadmap-corpus-survey.py`'s `KIND_VALUE` was anchored
`^\s+…$` — `rxKind()`'s blind spot, in the measuring instrument. Both defects
had to be fixed to see the data; reverting alone was not enough.

The survey now un-anchors with § 2.2's three guards (backtick lookbehind,
case-sensitive, last match), adds `+` to the value class for the three
compound values, and bounds a match at four words / 30 characters — reporting
prose matches rather than dropping them silently.

> **Verified** (measured against the pre-Phase-B2 source; the post-B2 figures
> are in the ANTS-4067 note below): the inventory lists `bug` at 29;
> non-canonical values 11 → 19,
> against the spec's seven additions; items with no `Kind:` 2,050 → 1,817. All
> seven newly-visible values occur in this project only. One spec figure was
> wrong and is corrected: `feature/fix` is 2, not 1.
>
> **Every `Kind:` figure taken before this fix is an undercount, corpus-wide** —
> including any quoted in `roadmap-data-model.md` § 7.4, and including the
> "1,613 with no Kind" in ANTS-4065's own roadmap bullet.

**B2. Fix the format defects the audit already found. DONE (2026-08-08).**

Counts here were originally scoped to open items; the file is conformed
throughout, because shipped bullets import under the same contract and INV-6's
round-trip covers every one of them.

- **41 headlines with no terminating period** (`roadmap-format.md` § 3.5
  requires one). Three different defects wearing one label: 20 had the period
  *outside* the bold, 15 used a `**Headline**:` label form where a period is
  wrong, and 6 genuinely lacked one.
- **297 headlines wrapped across lines** — § 3.5 requires the headline to
  "stand alone as a one-line summary". Not malformed prose, just hard-wrapped
  like the body. Unwrapped; the edit asserts the file's whitespace-normalised
  text is unchanged, so nothing moved but line breaks.
- **42 headlines over 200 characters**, split so the first clause stays as the
  headline and the remainder becomes body text. 41 are pure splits; ANTS-1670's
  headline was a four-item list, so it became a summary and the list moved into
  the body intact.
- The 99 bullets writing `Kind:` inline stay as they are — § 2.2 makes that
  shape supported rather than accidental.

> **What the 200-character limit actually is, since this plan implied
> otherwise.** It is `headlineProp["maxLength"] = 200` in the MCP tool's input
> schema (`src/claudeintegration.cpp:9111`) and nowhere else. The store column is
> `headline TEXT NOT NULL` with no length constraint, and neither the import nor
> the render checks length. **So none of the 42 was a migration defect** — they
> would all have imported, stored and rendered correctly. The limit binds only
> when a caller *passes* a headline to `roadmap_log`. They were conformed anyway,
> on the user's call, so the source is uniformly clean before the code changes.

> **Verified:** 0 headlines over 200 chars, 0 multiline headlines, 0 open
> headlines missing a terminator, 0 double-period artefacts, 1,709 distinct ids
> unchanged, `tools/check-roadmap.sh` clean.

> **Left alone, and flagged rather than fixed:** 11 headlines carry unbalanced
> parentheses or an odd number of backticks (ANTS-1530, 1615, 1702, 1987, 2039,
> 2046, 2048, 3423, 3569, 3672, 3803). Pre-existing — the count is identical
> before and after this work — and a different defect class from B2's three.
> An odd backtick is worth a look under § 2.2, whose guard is a backtick
> lookbehind.

**B3. Settle the open rulings. DONE (2026-08-08) — and one of them was never
open.**

**The compounds are resolved by correcting four bullets, not by a rule.**
Reading them says no rule should exist: the two `feature/fix` items are a bug
(ANTS-1219) and a feature (ANTS-1160), so any single mapping is wrong half the
time. `design + X` needs no judgement at all — `design` is not one of § 3.5.3's
21 values, so the other half is the only legal one. All four values occurred in
this project only; correcting the bullets removes them from the corpus, so no
compound mapping is added.

**`priority` was already decided, and the spec is wrong to have deferred it.**
`roadmap-data-model.md` **§ 7.5** is normative and states every part the spec
claims is missing: `priority` is "1 (highest) to 5 (lowest)"; `CRITICAL → 1,
HIGH → 2, MEDIUM → 3, LOW → 4`; band 5 reserved for someday-maybe; and § 3.3
already leaves it empty on migrated items. The spec's § 2.1 and § 5 say "a
severity vocabulary this project has never written down — no doc defines the
set, and the direction (is `CRITICAL` 1 or 5?) is a convention, not a
derivation." **Every clause of that is false against § 7.5.**

> **This is the same defect twice, and that is the finding worth keeping.** The
> spec's own cold-eyes loop 1 caught it on the `Kind` table — "§ 2.1 restated a
> mapping that already exists and is normative … and re-opened three of those as
> 'ruling needed'". The fix was applied to `Kind`; the identical defect sitting
> in the priority paragraph survived all three loops. A spec that re-opens a
> settled standard is a shape worth checking for directly, not once per lucky
> read.

**Corpus behaviour confirms § 7.5 rather than establishing it:** 86 of ~90
`Priority:` declarations are already integers 1–5, `Priority: 1` is an
in-progress security fix and `Priority: 5` a speculative idea. An absent
priority stays NULL — 3,304 of 3,392 bullets declare none, and defaulting them
would invent 3,304 values and mislabel ANTS-3853 (in-progress, standing top
priority) as least urgent.

> **Still owed, and it is documentation only — tracked as ANTS-4067:**
> § 7.4's "32 distinct values — 21 canonical plus 11 others" is now **21, all
> canonical, zero others** after this session's normalisation, and it never
> received the spec's four additions; the spec's priority deferral (§ 2.1, § 5)
> must be deleted in favour of a pointer to § 7.5; and figures measured with the
> pre-fix survey need refreshing (items with no `Kind:` is **1,814**, not the
> spec's 1,613). § 7.4 is a standard, so rule 14 gates the edit — run
> `/cold-eyes` on `roadmap-data-model.md`, which nobody has cold-read since the
> migration produced real evidence.

---

## Phase C — Implement the contract

Test-first throughout: the spec names six cases that **must red** on today's
source (§ 6). Write them, watch them fail, then fix.

**C1. `tests/features/roadmap_import_mapping/` — the fixtures.**
Cover INV-1…INV-11. Construct `RoadmapStore` with an **explicit path**; the
default resolves under `XDG_DATA_HOME` and would run the suite against the live
store.

> **Verify:** INV-2, INV-5, INV-9, INV-1, INV-7 and INV-10's equal-value fixture
> all fail. A green run here means the fixtures are wrong, not the code.

**C2. Parser — un-anchor `rxKind()`, add the backtick guard, drop
`CaseInsensitiveOption`, and take the last match** (§ 2.2).

> **Verify:** INV-2, INV-3, INV-9, INV-11 green. INV-10's three fixtures green.

**C3. Migrator — note every default; extend the mapping** (§ 2.1, § 2.3).
`field_defaulted` per defaulted field, `defaulted_fields` tally in the envelope,
plus the four mechanical additions (`bug`, `performance`, `process + tooling`,
`audit`) added to `mappedKind()` **and** § 7.4 together.

> **Verify:** INV-1, INV-4 green. A migration of the reverted roadmap reports a
> non-zero `defaulted_fields` and names the fields.

**C4. Render — suppress a defaulted `source`** (§ 2.4).
`assertedSource` is `provenance.source != "defaulted"` — not `== "asserted"`;
the spec explains why the direction is the whole finding.

> **Verify:** INV-5 green, including its `layman`/`lanes`/`evidence` clause.

**C5. Path validation** (§ 2.5) — the predicate, the note,
`extras.unresolved_path` as an array.

> **Verify:** INV-7 green.

> **Whole-phase gate:** `ctest --preset=default` green, and build `build/`
> current before any push — the pre-push hook runs ctest without building.

---

## Phase D — Re-migrate and prove it

**D2's and D3's checks are committed, not described** —
`tools/roadmap-import-verify.py` (declared kind vs stored kind) and
`tools/roadmap-roundtrip-diff.py` (per-column render→re-import diff). Both
take any project, and Phase E runs them per project. Read their module
docstrings rather than re-deriving the method from the prose below.

**The two SCRIPTS report and never write. The D3 PROCEDURE between them is
destructive, and that distinction is the one to carry into E2.** Step 2 drives
a real render, which rewrites `ROADMAP.md` and every rotated archive from the
store — and a bullet the store has not imported is DELETED rather than
reformatted. So the procedure is run against a COMMITTED, CLEAN tree, restored
with `git checkout`, and followed by a `roadmap_migrate` re-run or the store
keeps the rendered values. `roadmap-roundtrip-diff.py`'s own docstring states
all three; this plan did not, and E2 executes it against 13 projects.

**Which columns the gate actually covers, because three different sets are in
play and an executor has to pick one.** The spec's § 2.6 governs nine: `id`,
`status`, `headline`, `kind`, `source`, `layman`, `lanes`, `evidence`, `body`.
The instrument's `COLS` is also nine but not the same nine — it omits `id` and
`evidence` and adds `section_id` and `id_origin`. The D3 table below reports
seven. **The gate is the instrument's set**, with two riders: `evidence` is
NOT covered by it (the `item` table has the column; the tool does not read
it), and `id_origin` movement on the five re-run rows is [ANTS-4343], not
drift.

**D1. Dry-run the import against the reverted roadmap.**

> **Verify:** `items_orphaned` 0. Every `Kind: bug` bullet reports `kind='fix'`
> with `extras.source_kind='bug'`. No `field_defaulted` note names a field the
> bullet visibly declares — spot-check against every inline-`Kind:` bullet (99
> here, § 2.2). **Not 48**, which this step read until 2026-08-15: 48 was taken
> before B1's survey fix, and B1 establishes that every `Kind:` figure predating
> it is an undercount. Sized at 48 the check skips 51 bullets and reads clean.

**D2. Run it for real, then diff store against source.**

> **Verify:** no item's `kind` differs from the value its bullet declares. This
> is the check that would have caught the original 123 rewrites, and it is the
> one that says Phase C worked.

**D2 is DONE, 2026-08-13.** It refused the whole project once, on
`UNIQUE constraint failed: item.project_id, item.id_fold` — cause and fix
are [ANTS-4142] (commit `d7987e89`) — and ran clean on the first attempt
after the fixed binary was live: `ok`, 1980 items, **0 orphaned**, 1955
unchanged, 20 updated, 5 inserted, 5 ids allocated.

**The verify is green over every bullet, not a spot-check.** Comparing the
kind each bullet *declares* against the kind the store *holds*, across
`ROADMAP.md` and both rotated archives: **1434 bullets declare a kind, 0
mismatches, 0 declared-but-absent**. The four the ANTS-4086 resolver had
been getting wrong now read `ANTS-3814 investigate`, `ANTS-1278 chore`,
`ANTS-1866 doc-fix`, `ANTS-3608 doc-fix`. `field_defaulted(kind)` is 360 —
items whose bullet declares nothing, not items the import guessed at.

The five displaced synthesised ids landed as designed (`ANTS-4141`
… `ANTS-4145` were each filed by hand into the still-squatted
`4141…4337` block, so each took its id back and the id-less bullet that
held it was re-inserted at `ANTS-4338`…`4342`). One thing the re-run does
NOT do: those five store rows still carry `id_origin='synthesised'`
although their source bullets now declare the id — [ANTS-4343].

`roadmap_log` rewrites `ROADMAP.md` wholesale from the store ([ANTS-4141]);
`roadmap_query` answers from the store ([ANTS-4143], now labelled — ANTS-4402).

**SUPERSEDED 2026-08-15 — do not hand-edit this project's roadmap.** This
paragraph read "Keep editing the file by hand", conditioned on "Until D3 closes
the render drift". D4 closes by ACCEPTING that drift rather than removing it
(see below), so the condition could never discharge by its own terms and the
instruction stood permanently — against E2's status note and against "What this
plan deliberately does not do", which both say the file is generated output. A
hand edit now lands in a file the next render overwrites. See E2.

**D3. Render, then re-import — the acceptance test.** (It was INV-6's identity
bar; ANTS-4344 replaced that with the criterion below. The step is still the
acceptance test, just not that test.)

> **Verify (CORRECTED 2026-08-15 — see ANTS-4344):** the store is **idempotent
> after canonicalisation**. Cycle 1 may move any number of items — it is the
> one-time normalisation ANTS-3758 § 2.6 already accepts as "not data loss" —
> and **cycle 2 may move ONLY the write that drove the render**. Anything else
> fails the step.
>
> **One bounded exception, and it is an open defect rather than a licence:**
> `ANTS-1861`, whose `layman` text quotes the markup the renderer emits and so
> moves on every cycle. Filed as [ANTS-4405]; when that ships the exception is
> deleted from this step.
>
> **This wording is deliberately mechanical.** The first attempt read "cycle 2
> must move only items whose movement is individually explained", which nothing
> can fail — every mover admits an account after the fact, and `ANTS-1861` was
> passed on exactly that ground while moving on *both* cycles. A criterion that
> cannot separate a one-time canonicalisation from a permanent oscillator is not
> the criterion this step needs, and E2 runs it against 13 projects nobody has
> read.
>
> **This deliberately replaces `items_updated == 0` on cycle 1, which was the
> wrong criterion and not merely an unmet one.** Meeting it required suppressing
> the rendered `Kind:` line for a defaulted kind, and ANTS-3758 INV-12 forbids
> exactly that by name — every emitted bullet must carry the four pieces
> roadmap-format.md § 3.5 makes required, `Kind:` among them, and INV-12's own
> "Breaks when:" is written as "the renderer skips `Kind:` for items whose kind
> is `implement`, on the reasoning that the default restores it". The `Source:`
> precedent does not transfer: § 3.5.3 makes `Source:` optional with a
> documented default, while `Kind:` is required as of v1.1, so suppressing it
> emits a non-conforming bullet.
>
> **`headline` and `lanes` are NOT expected to fail** — the earlier text said
> they were, and the D3 run below measured both at 0. Earlier phases fixed them
> and § 4 of the spec was never corrected.

**D3/D4 result, 2026-08-13 — run, and the prediction above was wrong in both
directions.** Method: snapshot the store's governed columns, drive a real
render through `roadmap_log`, re-import, diff the store against its own
snapshot, then restore the files with `git checkout` and re-import to resync.
Cycle 1 over 1,980 items:

| column | moved |
|---|---|
| status, headline, kind, source, lanes | **0** |
| layman | 1 |
| body | 363 |

**Two of the three columns the spec named as expected failures do not fail
at all.** `headline` and `lanes` round-trip exactly; earlier phases fixed them
and the spec was never corrected. The spec states that expectation in § 2.6 and
defers `headline`/`layman` in § 5 — **not § 4**, which this paragraph cited
until 2026-08-15 and which is "RAM / build cost". `layman` moves on exactly one
item — `ANTS-1861`, whose layman text *quotes the markup pattern the
renderer emits*, so it is self-referential rather than a class.

**`body` is the whole story, it is one cause, and the spec never named it.**
The render writes `Kind: implement.` for an item whose kind was defaulted;
the re-import parses that line as the kind (hence `kind` moving 0) **and
also leaves it in the body**. Every one of the 363 diffs is that line and
nothing else. So the trailer-strip and the field-parse disagree about
whether a rendered `Kind:` line is body text.

**It converges — cycle 2 moves 2 items** (the marker annotate that drove
the render, and the `ANTS-1861` layman). It does not compound: the
duplicated line is appended once and is thereafter stable. That is the
distinction this step was told to make, and it comes out on the safe side.

**The severe finding was not drift at all** — the render deletes any bullet
the store has not imported. Recorded on [ANTS-4141] with the measurement.

**D4 is CLOSED, 2026-08-14 — as a wrong acceptance criterion, not a code
defect** ([ANTS-4344], commit `647cd8cd`). This step previously read "fix the
trailer-strip so a rendered `Kind:` line is not also body text, then re-run
both cycles", and that fix is the forbidden branch above: the only way to stop
the render emitting `Kind:` for a defaulted kind is to suppress it, which
INV-12 refuses. The property to hold is idempotence after canonicalisation,
and D3's own numbers satisfy it — cycle 1 moves 363, cycle 2 moves 2, and both
movers are explained rather than residual.

So Phase D is complete. No trailer-strip change is owed, and a future session
finding `body` moving on cycle 1 should read that as the canonicalisation
working, not as this step reopening.

---

## Phase E — Roll out

**E1.** Flip ANTS-4062 and ANTS-4063 to shipped; they are discharged by § 2.1
and INV-5 respectively.

**E1 is DONE, 2026-08-14.** Flipped ✅ for ANTS-4063 with
`RoadmapImportMapping` green (21/21, INV-5's `DefaultedSourceIsNotRendered`
among them); ANTS-4062 was already ✅. The `Kind:` half that bullet asked to
check became ANTS-4344, and it is ✅ too — resolved as a wrong acceptance
criterion rather than a code defect, which is what Phase D above now records.

**E2.** Migrate the remaining 13 projects, one at a time, each gated on D3's
round-trip check.

> **Verify:** per project, `items_orphaned` 0, and **D3's corrected criterion**
> — cycle 2 moves only the write that drove the render. **Not "INV-6 green"**,
> which this step required until 2026-08-15: INV-6 is § 2.6's
> `import(render(store)) == store`, the identity bar ANTS-4344 deleted, and
> cycle 1 moves `body` on 363 items even on the reference project. The
> instrument exits 1 whenever anything moved, so a gate wired to its exit status
> blocks all 13 projects on the outcome D3 now calls a pass. **Expect exit 1 on
> cycle 1; read cycle 2.**
>
> **Preconditions, from D3 above and not optional:** a committed, clean tree
> before starting; `git checkout` to restore; `roadmap_migrate` re-run
> afterwards. The render deletes bullets the store has not imported.
>
> **Expect the render gate first** — 2,141 corpus items carry no `Layman:`
> line, and a public open item without one refuses every write on that project
> until it is filled.

**E2 is UNBLOCKED as of 2026-08-15, and the blocker was never a policy
question.** The 446 orphans this phase stalled on were an artifact of
[ANTS-4403]: one line of prose at `ROADMAP.md:31081` opened a fence nothing
closed, so 481 bullets never reached the plan and every store row they would
have matched was reported unmatched. With the fence rule taken from
`MarkdownScan`, a dry run reports **0 orphaned**.

**This project is re-imported and current as of 2026-08-15** — 0 orphaned, 61
inserted, 1,969 unchanged, 12 updated, 0 ids allocated; the store now holds
2,042 items to `ANTS-4404`. That closes the read-side divergence [ANTS-4402]
named and clears the precondition [ANTS-4141] set for `roadmap_log`: the
divergence guard passes, so the verb writes rather than refusing, and the
hand-edit workaround is no longer required on this project.

One caveat a per-project run should expect, found the same day and fixed as
[ANTS-4404]: the write path's own fence walkers carried the same naive
predicate, which made 391 of this roadmap's 2,032 bullets refuse
`anchor_unsafe_context`. Any project whose roadmap quotes fence syntax in
prose will hit it on a binary older than that fix.

---

## What this plan deliberately does not do

- **It does not edit `ROADMAP.md` after Phase D.** Once re-migrated, the file is
  generated output; corrections go into the store.
- **It does not fix the pass-headings status vocabulary** (142 values outside
  the enum). That is a second dialect and a separate item.
- **It does not back-fill `Kind:`** onto the 1,814 corpus items that carry none.
  They default legitimately; Phase C's note makes the default visible, which is
  all this work owes them.

---

## Cold-eyes loop log

<!-- review-contract writes one row per review loop as it closes. -->

| Loop | Date | Lanes | Q1/Q2/Q3/Q4 | Outcome |
|---|---|---|---|---|
| 1 | 2026-08-15 | 3, cold — genre pinned `plan`, cap 2. Packet carried INV-12, ANTS-3758 § 2.6, roadmap-format § 3.5.3 and ANTS-4065 § 2.6 verbatim, plus the day's measured migration state | **Q1 1 · Q2 3 · Q3 2 · Q4 1** — verified 7, fixed 7, dismissed 1, out-of-scope 1 | **First gate on this plan, run because ANTS-4344 amended Phase D's acceptance criterion and said so rather than doing it unilaterally. All three lanes independently found the same three defects**, which is the run's strongest signal. **[Q2] "Keep editing the file by hand" was permanently live**: it was conditioned on "Until D3 closes the render drift", and D4 closes by ACCEPTING that drift, so the condition could never discharge — while E2 and "What this plan deliberately does not do" both say the file is generated output. An implementer reading Phase D in order would hand-edit and lose it to the next render. **[Q4] The criterion I had just written was unfalsifiable** — "cycle 2 must move only items whose movement is individually explained". Lane B produced the proof: `ANTS-1861` moves on cycle 1 *and* cycle 2, so it is a permanent oscillator, and it passed because its movement was explained. A criterion that cannot separate canonicalisation from oscillation is not the one this step needs. Now mechanical (only the write that drove the render), with `ANTS-1861` as a bounded exception filed as [ANTS-4405] rather than a standing note. **[Q2] 48 vs 99 inline-`Kind:` bullets** — D1's spot-check was sized at 48, the spec says 99 (§ 7, "1,435 own-line against 99 inline"), and B1 of this very plan establishes that every pre-fix `Kind:` figure is an undercount. Sized at 48 the check skips 51 bullets and reads clean. **[Q2] E2 gated on "INV-6 green"**, which is the identity bar ANTS-4344 deleted; the instrument exits 1 whenever anything moved, so a gate wired to it blocks all 13 projects on the outcome D3 now calls a pass. **[Q3] Three different column sets** were in play — spec § 2.6's nine, the instrument's different nine, and the D3 table's seven — with no statement of which the gate uses; `evidence` is governed but not read by the tool, and `id_origin` movement is ANTS-4343. **[Q3] E2 carried none of D3's destructive preconditions** (committed clean tree, `git checkout` restore, `roadmap_migrate` re-run), though the render deletes bullets the store has not imported and E2 runs it against 13 projects. **[Q1, found by the orchestrator while building the packet]** the plan cited "the spec's § 4" for the expected-drift statement; the spec states it in § 2.6 and defers in § 5, and § 4 is "RAM / build cost". **Dismissed, unverified:** lane A predicted `roadmap_log op:append` would reissue a live id because `id_prefix.high_water` (4342) trails the store's max item (4404). Re-run rather than reasoned — a dry-run append allocated `ANTS-4405`, correctly, because the floor is `max(counter, corpusHighWater)` and the corpus scan sees 4404. The packet fact invited the inference; the allocator is not reading that column. **Out of scope, fixed in place:** `tools/roadmap-roundtrip-diff.py` attributed its governed-column list to "ANTS-3765 § 2.4", which is that spec's list of nineteen store methods and defines no columns at all (both lanes B and C raised it as an open question). Doc 406 → 472 lines, this log included. |
