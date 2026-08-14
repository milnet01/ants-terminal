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
report and never write, both take any project, and Phase E runs them per
project. Read their module docstrings rather than re-deriving the method
from the prose below.

**D1. Dry-run the import against the reverted roadmap.**

> **Verify:** `items_orphaned` 0. Every `Kind: bug` bullet reports `kind='fix'`
> with `extras.source_kind='bug'`. No `field_defaulted` note names a field the
> bullet visibly declares — spot-check against the 48 inline-`Kind:` items.

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

Until D3 closes the render drift, `roadmap_log` still rewrites
`ROADMAP.md` wholesale from the store ([ANTS-4141]) and `roadmap_query`
answers from the store while reporting `path: ROADMAP.md` ([ANTS-4143]).
**Keep editing the file by hand.**

**D3. Render, then re-import — the acceptance test (INV-6).**

> **Verify:** `items_updated == 0` over the nine governed columns and no
> `field_conflict` naming one. **`headline`, `layman` and `lanes` are expected to
> fail here** — they are the spec's named undiagnosed drift, and this run is the
> instrument for diagnosing them. Anything *else* failing is a Phase C defect.

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
at all.** `headline` and `lanes` round-trip exactly; earlier phases fixed
them and the spec's § 4 was never corrected. `layman` moves on exactly one
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

**D4 remains open** on the `body` cause: fix the trailer-strip so a rendered
`Kind:` line is not also body text, then re-run both cycles. Fold into
ANTS-3758 (`§ 2.x`, re-gating per rule 14) rather than ANTS-3765 — the
render emits the line, and the parse is doing what it was told.

---

## Phase E — Roll out

**E1.** Flip ANTS-4062 and ANTS-4063 to shipped; they are discharged by § 2.1
and INV-5 respectively.

**E2.** Migrate the remaining 13 projects, one at a time, each gated on D3's
round-trip check.

> **Verify:** per project, `items_orphaned` 0 and INV-6 green before moving to
> the next. **Expect the render gate first** — 2,141 corpus items carry no
> `Layman:` line, and a public open item without one refuses every write on that
> project until it is filled.

---

## What this plan deliberately does not do

- **It does not edit `ROADMAP.md` after Phase D.** Once re-migrated, the file is
  generated output; corrections go into the store.
- **It does not fix the pass-headings status vocabulary** (142 values outside
  the enum). That is a second dialect and a separate item.
- **It does not back-fill `Kind:`** onto the 1,814 corpus items that carry none.
  They default legitimately; Phase C's note makes the default visible, which is
  all this work owes them.
