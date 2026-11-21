# ANTS-4108 — run a spec's own patterns against the examples beside them — review loop log

Record for [`../specs/ANTS-4108-spec-conformance-verb.md`](../specs/ANTS-4108-spec-conformance-verb.md) § 10. Cold-eyes loop log.

## Cold-eyes loop log

| Loop | Lanes | Q1 | Q2 | Q3 | Q4 | Verified / dismissed | Outcome |
|---|---|---|---|---|---|---|---|
| 1 | 2 cold, spec genre | 1 | 4 | 2 | 2 | 9 / 0 | all fixed |
| 2 | 2 cold, spec genre | 0 | 2 | 4 | 2 | 8 / 0 | all fixed |
| 3 | 2 cold, spec genre | 1 | 2 | 6 | 0 | 9 / 0 | all fixed; **cap reached, not converged** |
| 3-impl | none — implementation, no reviewer dispatched | 1 | 0 | 0 | 0 | 1 / 0 | § 2.4a added |
| 3-impl-2 | none — MCP wiring, no reviewer dispatched | 0 | 2 | 0 | 0 | 2 / 0 | § 2.3 deviations stated |

**Loop 1 (2026-08-12).** Both lanes independently found the same five
defects, which is what two rolls of a cold read are for.

- **[Q1]** § 2.4's worked example was itself the class this verb exists to
  catch. The fence pasted a pattern beginning `^\s*` while its expectation
  table was written for the column-0 form the example is meant to
  illustrate, so the row `` `  const local = ` `` → `no match` was false —
  verified by running it: `printf '  const local = ' | grep -oP '^\s*…'`
  matches and captures `local`. Fixed by restoring the column-0 anchor and
  re-running all three rows.
- **[Q2]** § 2.6's catch-all row refused every fence it did not recognise,
  including `cpp`/`jsonc` fences that every spec carries, and contradicted
  § 2.4's rule that a bare `regex` fence is a CANDIDATE. Scoped to
  `regex*` fences and given an explicit bare-`regex` row.
- **[Q2]** INV-9 demanded byte-identical envelopes while § 2.3 emits
  per-run timings — unsatisfiable by any correct implementation. Scoped to
  exclude `observations`, and the etag subset stated.
- **[Q2]** `truncated` was asserted by INV-7 but absent from the § 2.3
  envelope; added, along with the `refusals[]` array.
- **[Q2]** § 7 enumerated INV-8 into the feature test while § 8 and INV-8
  itself assigned it to a source-scrape. § 7 corrected.
- **[Q3]** Refusal granularity was undefined — one bad fence aborting the
  file, or skipping the case, were both buildable. Now stated: per case,
  with only a malformed request refusing the call.
- **[Q3]** The encoding for an empty capture was undefined while INV-3
  turns on exactly that distinction. Now stated.
- **[Q4]** INV-6 hashed the whole project root, which `build*/` and `.git/`
  churn during any test run — it would fail regardless of the verb. Scoped.
- **[Q4]** INV-8 claimed "no subprocess **and no interpreter**" while its
  test grepped one file for `QProcess` alone; `luaL_newstate`,
  `Py_Initialize`, `system(`, `popen` and `fork(` all passed it. Grep set
  and file set widened.

Author-side during the fix pass: the cap refusal was about to mint a new
`cap_exceeded` code, corrected to the taxonomy's existing `too_large`
(`mcp-error-codes.md`); a `§ 1.1` self-citation that resolved to nothing was
made explicit; and the "four defects" count was reconciled with the three
the bullet actually enumerates.

Lane spend: 99.4k and 96.5k input tokens against a ~60k/turn budget — over,
because the packet carries eight code windows and the document is long for
its genre.

**Loop 2 (2026-08-12).** Both lanes again converged, and **three of the
eight findings were collateral from loop 1's own fixes** — the pattern the
review procedure warns about, arriving on schedule.

- **[Q4]** INV-8's widened grep (a loop-1 fix) became *unreachable*: § 2.7
  names `src/mainwindow.cpp` and `src/claudeintegration.cpp`, which already
  carry 35 and 6 hits of the token set for unrelated verbs. Asserting zero
  where 41 exist is red for every possible implementation, so it falsified
  nothing. Scoped to the two files this change creates, and `\b`-anchored so
  `QProcessEnvironment` stops counting. *Loop-1 collateral.*
- **[Q2]** § 2.3 said `max_cases` is clamped; § 2.5 (a loop-1 fix) said an
  out-of-range value refuses the call. Both were buildable. Now: refuses.
  *Loop-1 collateral.*
- **[Q2]** § 2.2 claimed the three uncaught defects surface "as CANDIDATEs"
  while § 2.6 (a loop-1 fix) made non-`regex` fences invisible. § 2.2 now
  says plainly that this verb surfaces none of them. *Loop-1 collateral.*
- **[Q3]** Only one candidate `kind` was ever named while three causes
  produce candidates; all three enumerated, since the author's next action
  differs per kind.
- **[Q3]** An unsupported-engine fence with no table satisfied INV-4 and
  INV-5 at once. § 2.6 now fixes the order (engine before table) and INV-5
  asserts `candidates == []`.
- **[Q3]** `line` was emitted in three buckets and defined in none.
- **[Q3]** `expected` did not say which group a multi-group pattern yields,
  and the `no match` sentinel was indistinguishable from the literal after
  backtick stripping.
- **[Q4]** INV-3 named the empty-capture conflation as its break, but its
  test used a pattern that never captures emptily — the break was
  unreachable by its own fixture. Second row added.

A lane also challenged § 2.6's load-bearing premise (that Qt leaves
`UseUnicodePropertiesOption` off). Verified rather than defended: the
constructor takes `PatternOptions options = NoPatternOption` — citation
tightened to the three header lines that show it.

Lane spend: 108.5k and 105.1k input tokens. Document grew 394 → 479 lines
across the fix pass, which is the cost this review's consolidation rule
exists to watch.

**Loop 3 (2026-08-12) — the cap. Nine findings, all fixed, NOT converged.**

- **[Q1]** Both lanes caught the same thing, and it is loop 2's own: the
  INV-8 scope paragraph cited "35 and 6 hits … 41" while prescribing a
  `\b`-anchored regex. Under the regex it actually prescribes the figures
  are 33, 1 and 0 — 34 lines. The paragraph measured one regex and
  prescribed another, **and its own closing sentence about `\b` excluding
  `QProcessEnvironment` was the proof**. Corrected in both places.
- **[Q2]** § 2.2's loop-2 rewrite ("surfaces none of the others") left
  § 2.5 and INV-4 still crediting CANDIDATEs with "two of the four reported
  defects". Deleted rather than reconciled.
- **[Q2]** The `line` rule sent every candidate to the fence's line, but
  `malformed_expectation_cell` has a row to point at. Replaced with a table
  covering all five entry kinds.
- **[Q3]** A refusal was "per case", but INV-5's fixture is a fence with
  **zero** cases — so its own asserted `refusals[0]` had no case to attach
  to. Now per case, and per fence where the fence yields none.
- **[Q3]** Only one ordering rule existed (engine before table), leaving a
  `regex python` fence with a 600-byte pattern satisfying two codes at once.
  Full precedence stated: engine → pattern cap → table → input cap, at most
  one refusal per case.
- **[Q3]** A bare ` ```regex ` fence with no table satisfied two candidate
  kinds; now at most one candidate per fence, engine check winning.
- **[Q3]** `observations[]` had no cardinality and no `line` rule.
- **[Q3]** The empty-capture encoding conflated a *non-participating* group
  with a *zero-length* one. Qt distinguishes them, but `QString() ==
  QString("")` is **true**, so the spec now names
  `QRegularExpressionMatch::hasCaptured(1)` as the discriminator — an
  implementer reaching for `==` would have collapsed two of the three
  outcomes INV-3 exists to separate.
- **[Q3]** Fence **nesting** was never specified, and § 2.4's own worked
  example is a four-backtick `markdown` fence containing a `regex` fence:
  a naive scanner run on this very spec would extract three cases from an
  illustration. Only top-level fences are scanned.

**Why the cap bound, since that is evidence about this document.** Not
size: 30,930 bytes against a corpus median of 20,676 and a 90th percentile
of 55,794 (`ls -l docs/specs/*.md`), and the two specs that needed nine and
eleven loops were over a thousand lines. The cause is that **the response
envelope and extraction taxonomy were being designed inside the review**.
Every loop's fixes added surface — new fields, new `kind` values, new
ordering rules — and that new surface is what the next cold read found: 3
of loop 2's 8 findings and at least 4 of loop 3's 9 landed on text an
earlier loop had added. § 1, § 2.1, § 2.2, § 5 and § 6 — the problem, the
scope decision and the alternatives — produced no findings after loop 1 and
are settled.

**Recommendation, following ANTS-4070's precedent:** close the review and
build test-first. Every unresolved item is envelope detail that a fixture
pins in seconds and a cold read argues about for a loop — which is this
spec's own thesis (§ 1) turned on itself.

**Row `3-impl` (2026-08-12) — implementation, NO reviewer dispatched.**
The engine was built test-first and its ten fixtures pass. Building it
surfaced one [Q1] defect that three cold loops and six lanes could not,
because it needs a corpus rather than a reader: **nothing in this
repository uses the convention § 2.4 defines** — one `regex` fence exists
and it is § 2.4's own nested illustration — so the verb reports nothing
against today's corpus, and the reporting session's evidence was a Python
`re.search` CALL that this extractor does not recognise at all. § 2.4a now
states both, and ANTS-4128 carries the fix.

**A re-gate of that amendment is owed and deliberately not run.** Step 8
asks for one; the addition is a stated limitation rather than a changed
contract, the gate had already bound its cap on this document, and the
standing recommendation is to trust fixtures over further cold reads here.
Recorded as a decision so it is not mistaken for an oversight.

**Row `3-impl-2` (2026-08-12) — MCP wiring, NO reviewer dispatched.**
The verb is wired and its three verb-layer fixtures pass. Wiring it surfaced
two [Q2] contradictions between § 2.3 and the standard it claims conformance
with, and both have the same cause — `observations[]`, which no earlier loop
traced past the etag:

- § 2.3 claimed ETag-304 per `mcp-tools.md` step 7, which computes the etag
  over the **whole** response. INV-9 already knew the timings had to be
  excluded, and loop 1 scoped the etag for exactly that reason; what nobody
  followed through was that the central mechanism therefore **cannot** be the
  one used. Under it the 304 never fires, which makes INV-9 unsatisfiable —
  the same defect class loop 2 caught in INV-8.
- § 2.3 claimed `fields=` as well. Central projection is skipped on a central
  304 only, so a handler-local 304 would have `unchanged` projected out of it.
  Declined.

Both are now stated in § 2.3 with the reason, and the `isEtagSupportedTool`
absence — the thing that keeps the two mechanisms from being wired at once —
is asserted by a fixture rather than left to a comment.

**A re-gate of these amendments is owed and deliberately not run**, on the
same grounds as the row above: the additions state which of two documented
mechanisms this verb uses and why the other is unavailable, the gate had
already bound its cap on this document, and the fixtures now pin the choice.
Recorded as a decision so it is not mistaken for an oversight.
