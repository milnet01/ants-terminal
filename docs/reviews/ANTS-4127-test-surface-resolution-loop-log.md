# ANTS-4127 — test surface resolution: review loop log

## 11. Cold-eyes loop log

| Loop | Lanes | Q1 | Q2 | Q3 | Q4 | Verified / dismissed | Outcome |
|---|---|---|---|---|---|---|---|
| 1 | 2 cold, spec genre | 7 | 4 | 4 | 1 | 16 / 0 | all fixed |
| 2 | 2 cold, spec genre | 3 | 3 | 4 | 1 | 11 / 0 | all fixed; + 4 collateral in own sweep |
| 3 | 2 cold, spec genre | 3 | 3 | 2 | 2 | 10 / 1 | all fixed; **cap reached, not converged**; empty tail |
| impl | 0 — no reviewer dispatched; an in-place amendment | — | — | — | — | — | 2026-11-02, ANTS-5273: adds INV-11. Rule 14: an amendment gets no automatic review. |
| impl-6765 | 0 — no reviewer dispatched; an in-place amendment | — | — | — | — | — | 2026-11-15, ANTS-6765: § 2.4 — a path after `citing`, `cites`, `naming` or `names` is a fixture's data and is not harvested. Rows moved out of the spec into this file. Test seen red first; corpus test_surface_absent 1 to 0, test_surface_unresolved unchanged at 3. |

**Loop 1 (2026-08-12).** 12 of the 16 came from the two lanes (2 raised by
both, merged on defect identity); 4 were found by the orchestrator — two
while building the context packet, one while verifying a lane's arithmetic,
one while writing a fix.

- **[Q1] The engine cannot do what § 2.6 asked it to.** Found building the
  packet, before a lane was spent: `src/speclint.h` states twice that
  `SpecLint::check()` is handed the document's *text* and "cannot find or
  read that file either", yet § 2.6/§ 2.8 had it `stat`-ing directories and
  parsing `CMakeLists.txt`. Re-shaped onto the injection contract
  `requiredSections` already uses — which is also what made every invariant
  drivable from `Options`.
- **[Q1] Every count in the document stopped reproducing when the document
  was saved.** The figures were measured before this file joined
  `docs/specs/`; it carries nine `*Test:*` clauses of its own, so the quoted
  commands returned 1039 against a stated 1026. Fixed with
  `--exclude='ANTS-4127-*'` on every count and § 2.4b stating the
  reflexivity outright. Neither lane could see this — both were briefed with
  the pre-existing measurements.
- **[Q1] The invisible-clause figure was measured by proxy.** Both lanes
  independently found 766 understated it. Verifying showed the honest
  denominator is the extraction pattern itself: 986 of 1026, not 766 — the
  proxy counted a clause as visible for merely mentioning a backticked name
  or a `tests/unit/…` path.
- **[Q2] INV-5 and INV-6 contradicted each other.** A `superseded` spec
  citing a present-but-unwired directory was covered by both, oppositely.
  Resolved by skipping abandoned specs *before* either check rather than
  per-check.
- **[Q4] The fix for INV-9 shipped a tautology, caught while writing it.**
  The first draft asserted zero findings in all three arms — which passes
  against an engine that never runs either check. Re-cut so arm (a) and arm
  (b) differ only in whether the set was empty.
- Also fixed: `surfaces_resolved` was defined over clauses but named for
  surfaces (Q3); `spec_status` had no defined value for the 53 Status-less
  specs (Q3); `existingTestDirs` non-empty with `wiredTestDirs` empty was
  unspecified and would have filed a corpus-wide false-positive run from one
  failed file read (Q3); INV-8's source-grep returns 1 today against correct
  code, on a comment in `src/speclint.cpp:40` (Q1); § 2.1 described the walk
  as bullet-only when it covers the table form too (Q1); the § 4 memory
  budget used the cited-name population (266) where the injected set is the
  on-disk one (499) (Q1); § 10 annotated the ROADMAP bullet's *home* but not
  its headline, which still promises fixture execution (Q2); and ANTS-4108
  § 6 defers **two** bullets to this id, of which only one was accounted for
  (Q2).

The document grew 452 → 558 lines this loop, against the delete-first rule.
Two invariants and one subsection are genuine Q3/Q1 gaps that added where
nothing stood; the remainder replaced text rather than sitting beside it.

**Not machine-checked, and not to be read as clean:** `spec_lint` returns
`sections_checked: false` — when this spec was written no format standard in
this project carried a `<!-- required-sections -->` block (ANTS-3829 has since
added one to `docs/standards/specs.md`), so required-section structure was
verified by hand against `docs/standards/specs.md` § 3 + § 4, not by a tool.
**This is not specific to this spec: the check has never run on any spec in
this corpus**, and the missing block is tracked as **ANTS-3829** (with
**ANTS-4080** for the global-standard half). Until one of those lands, every
`spec_lint` clean result in this project is silent about section structure,
and a gate reporting it as settled is reporting something nobody measured.

**Loop 2 (2026-08-12).** 8 of the 11 came from the two lanes (3 raised by
both); 3 more surfaced from lane open questions the orchestrator resolved.
A fourth open question — whether `tests/features/spec_lint_verb/` is really
wired, since the quoted `grep -c` pattern is a prefix of it — resolved
**clean** at `CMakeLists.txt:1499-1500`, and is recorded here because a
question that came back sound is evidence the packet held.

- **[Q1] The yield measurement was line-scoped, and a clause is not a
  line.** Invariant bullets hard-wrap, so `grep -ohE '\*Test:\*.*'` stops
  before a path on the continuation line — and INV-1 in this document is
  exactly that shape. Re-measured clause-aware: **137 harvestable of 1018,
  so 881 invisible**, against the 40/986 loop 1 had settled on. The engine
  reads the parser's joined `test_surface`, so 137 is what it will see.
  **This is the second time this one figure has been wrong in two loops**,
  by two unrelated mechanisms; § 2.4 now records both, because the next
  person to re-measure it needs to know which traps are already known.
- **[Q3] The wiring check had no `kind`.** § 2.6 and INV-6 created a third
  outcome — a directory that exists but is in no bundle — and § 2.3 defined
  only two names for it. An implementer would have emitted
  `test_surface_absent` for a directory that is right there. Now
  `test_surface_unwired`, with its own example.
- **[Q2] Three of loop 2's findings were loop 1's own collateral**, and
  they are the pattern worth naming: § 2.6 still said "regardless of
  Status" after INV-6 gained its INV-5 carve-out; § 2.7's `Result` row
  still listed one new member after § 2.3 grew a second; and § 2.3's null
  population said 53 where § 2.5 had already split it into 53 + 3 = 56.
  Each fix was correct and each left its other half behind.
- Also fixed: `surfaces_checked` was undefined for the one row § 2.6 calls
  "the one that matters" — `existingTestDirs` non-empty, `wiredTestDirs`
  empty — and INV-10 tested only the two arms either side of it (Q2);
  "distinct surfaces" never said distinct *within what*, so two conforming
  engines could report different totals for one corpus (Q3); INV-1 asserted
  "attempts", which the envelope does not emit (Q4); § 2.1 described
  ANTS-4108's taxonomy as "FINDING, CANDIDATE, and silently invisible" when
  it has three named buckets including OBSERVATION (Q1); and
  `wiredTestDirs ⊆ existingTestDirs` was never stated (Q3).

**Collateral caught in this loop's own sweep, not by a lane (4).** The
header still described the inherited taxonomy the way § 2.1 no longer did;
§ 2.3's prose still said "two `kind` values … one counter"; and **two count
commands lacked the `--exclude` § 2.4b requires** — § 1's, which now reads
268/28 because this document cites two deliberately-unreal example
directories, and § 2.5's, which now reads 241/188 because this document is
itself a spec carrying a Status line. The reflexivity § 2.4b describes is
not a one-time correction; it re-arms on every edit that adds an example.

**Loop 3 (2026-08-12) — the cap.** 10 verified, 1 dismissed. Three lane
open questions resolved **clean** (ANTS-4108 § 2.2's "not even a CANDIDATE"
wording; `tests/features/spec_lint_verb/` really is wired; `QFile`/`QDir`/
`system(`/`popen`/`fork(` appear nowhere in `src/speclint.{h,cpp}`, so
INV-8's comment carve-out is load-bearing for `QProcess` alone).

- **[Q4] INV-10's own falsifier was misattributed** — the clause claimed
  arms (a) and (c) ensure "neither field alone separates them", but an
  implementation reading `surfaces_checked = (surfaces_resolved > 0)`
  returns false/0 at (a) and true/1 at (c) and **passes both**. Arm (b) is
  the only falsifier. A test author trusting the sentence would have shipped
  a suite that could not catch the defect the invariant exists for — the
  same green-by-construction shape § 1 is about, in this document's own
  contract, at the third loop.
- **[Q2] The first-run yield was counted per spec where findings are
  emitted per surface.** § 1's table lists three absent surfaces in two
  shipped-status specs (ANTS-1111 cites two), so § 6's "2 defects / 21
  noise" was wrong on both halves. Re-measured per (spec, surface) pair:
  **25 pairs — 3 FINDINGs, 20 CANDIDATEs, 2 invisible**, so the rejected
  alternative is 88% noise, not 91%.
- **[Q3] `spec_status` normalisation was under-specified.** § 2.3 said
  "lowercased first word"; § 2.5's own pipeline also strips leading `*` and
  truncates at the first non-`[a-z0-9]`. Without all three, `**considered`
  and `accepted,` match no row and fall to the catch-all — a CANDIDATE
  where INV-3 requires a FINDING, silently. INV-4 gained a fixture per step.
- **[Q4] Per-document distinctness had no test.** § 2.3 pinned the scope in
  loop 2 and no clause reached it; an engine deduplicating per clause passed
  every invariant and still reported a different corpus total.
- Also fixed: "two new finding kinds" survived in § 2.7 and § 10 after § 2.3
  grew a third (Q2); § 2.4b cited a figure (1026) the document no longer
  states (Q1); § 8's verb lane omitted `surfaces_checked`, the one field
  § 2.3 calls "always emitted" (Q2); "nine `*Test:*` clauses of its own"
  where there are ten (Q1); INV-5 was silent on what a skipped spec
  contributes to `surfaces_resolved` (Q3); and § 6's `cpp`-fence count
  needed the `--exclude` too, this document's own § 2.6 snippet having made
  it 443/152 (Q1).

**Dismissed (1).** A lane reported the document's invariant count as
disagreeing with the briefing packet's "9". The document carries ten and
`spec_query` returns ten — the **packet** was stale, written before INV-10
existed. Recorded rather than dropped, and the lane was right to dispute a
packet fact rather than defer to it. **Collateral (1):** a blank-line
paragraph added inside INV-10 split the bullet, so `spec_query` parsed the
new prose as part of the *body* and left the test surface truncated —
caught by re-running the parse check after the fix, not by reading.

**Why the cap bound, and it is not size.** At 781 lines this spec sits
above the corpus median (392) but below the 90th percentile (822), with 32
of 241 specs larger; the documents that historically needed nine and eleven
loops were over 1000. The cause is that this document is unusually
**measurement-dense** — nearly every section rests on a corpus count, and
each count carried a distinct population trap: proxy versus pattern, line
versus clause, spec versus surface, and the corpus changing because this
file joined it. The single figure "how many clauses are invisible" was
wrong in three consecutive loops by three unrelated mechanisms. Those are
Q1 defects a cold read surfaces one at a time, and splitting the document
would not have found them faster — the measurements are the contract.
**The tail is empty: every finding from all three loops is fixed.**
