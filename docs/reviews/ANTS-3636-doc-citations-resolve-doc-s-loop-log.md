# ANTS-3636 — `doc_citations`: resolve a doc's `path:line` citations and return the cited text — review loop log

Record for [`../specs/ANTS-3636-doc-citations-resolve-doc-s.md`](../specs/ANTS-3636-doc-citations-resolve-doc-s.md) § Cold-eyes loop log.

## Cold-eyes loop log

<!-- doc-examples: begin -->

Loops 1–10 ran three lanes each (design / accuracy / conformance), re-briefed
cold every loop, all on 2026-07-26. Entries below are compressed to the counts
and the findings that changed the design; loop 11 rewrote the document, so its
entry is kept in full.

**Loop 1** — **C4 · H7 · M12 · L12 · I3**, 35 verified / 1 dismissed.
`CodebaseIndex::serve` returns a `QJsonObject`, not an `Index`, so the named
reuse could not have worked; the `unparsed[]`/`unresolved`/`missing_file`
three-way contradiction was resolved by making the ladder total;
`ReadRegion::extract` was dropped as a reuse. Load-bearing: § 2.5's anchor idiom
had been asserted "dominant" and measures 22% — `only:"stale"` as first drafted
would have dropped the unanchored majority of healthy citations.

**Loop 2** — **C6 · H6 · M14 · L16 · I3**. Loop 1's symlink guard was a false
security claim: `QFileInfo::isSymLink` inspects only the final component, so
`src/link/foo.cpp:1` would have been read. Replaced with gate G. Also: `counts`
had no bucket for `unparsed[]`, so a cold index was invisible.

**Loop 3** — **C3 · H8 · M11 · L22 · I3**, 47 verified. All three lanes found
the same registration-file error. Two design holes: the continuation grammar had
no left-context rule, so `09:45` in prose would have inherited an unrelated path
and emitted that file's real line 45 as `ok`; and INV-17 promised a resumable
`next_offset` for a cap that truncates the *scan*. Also the RAM budget counted
on-disk bytes for a cache of UTF-16 `QStringList`s (~3× understated).

**Loop 4** — **C4 · H5 · M12 · L20 · I5**, 46 verified. `ROADMAP.md` was 27,024
lines, not the ~19.5k claimed, so the 20,000-line guard silently truncated the
very file used to justify it — and INV-31 had just made that truncation
unresumable. Fixed by making `max_doc_lines` a request argument. The grammar had
no leading-`/` production, so step 0 was unreachable and INV-6 unsatisfiable.
Three invariants were specified as verb-level calls against a handler needing a
live MainWindow; ANTS-3601 had already solved this with extracted pure statics.

**The finding that changed how this spec is maintained.** Six of loop 4's
defects were introduced by *loop 3's own fixes*, every one the same shape: a
passage was edited and the passages referencing it were not. So loop 4's
response was structural: the document was consolidated, each rule stated once,
and every fix written against an explicit ledger of "what else must agree with
this". Loops 1–3's habit of adding prose to defend each fix was itself the
mechanism generating the next loop's findings.

**Loop 5** — **C4 · H6 · M10 · L22**, 42 verified. The ledger worked, partially:
the edited-a-passage-and-left-its-neighbour class produced one finding instead
of half. That one defined a new rule — loop 4's fix had split the tests across
two bundles citing ANTS-3601 as precedent *without opening ANTS-3601*, which
uses **two** feature directories where the fix specified one. Hence: *when a fix
cites a precedent, open the precedent*. Three findings were nobody's regression:
nothing capped an emitted `text` line, so one minified line defeated `max_bytes`
while INV-35 required at least one entry (now INV-43); `unterminated_fence` was
specified as derivable from `fenceMask`, which returns bare bools and masks the
closer line true as well (now a `fenceMask` overload + INV-36); and `codeSpans`
was specified line-scoped, which would have re-introduced ANTS-3635(a) (now
whole-document + INV-44). Also: the MCP envelope sanitises `<ants_mcp_data>` and
`<!--`/`-->` out of every payload, so cited text arrives altered (now
`text_escaped` + INV-45); and the anchor coverage figure was 22% *of one scan's
denominator*, which against the real harvest is 20%.

**Loop 6** — **C5 · H4 · M12 · L15 · I2**, 36 verified / 1 dismissed. The first
clean accuracy pass in six loops. The design lane found four contradictions, two
introduced by loop 5's own fixes: the directory-citation rule was unreachable
for every example it gave, and the "~6 KB worst case per entry" sized itself on
the *default* range length rather than the clamp ceiling, understating by up to
7× — resolved by stating that `max_bytes` is a soft ceiling with one bounded
overshoot. The conformance lane found the loop's most expensive error, a factual
claim about the test suite never checked: a missing `selection_hint` does **not**
degrade silently — two feature tests fail on it, and two further format rules are
enforced project-wide and documented nowhere (ANTS-3645). The same lane rejected
a second inline-span scanner: `DocIntegrity::maskInlineCode` already is that
algorithm, so `codeSpans` became a hoist. Method rule added: **"no test covers
this" is a claim about a file and needs the same grep as "a test covers this."**
The reconciliation pass also caught an unterminated code fence left by a grammar
fix — invisible to a reader, obvious to a scanner, in a document whose subject is
that class of defect.

**Loop 7** — **C4 · H16 · M26** raw (~65 unique), the largest yet, first loop
after the consolidation. A pre-pass fixed six prose-wrap defects, every one a
replace-edit that joined onto a surrounding line fragment — the editing tool
reports success, because the replacement does happen. `anchor_found` was
specified three ways, and it is what `only:"stale"` filters on, so a literal
reading inverted the filter into an all-clear. INV-18 specified a
`caller_cwd_required` branch that cannot fire while omitting the
unresolvable-root refusal that does. § 2.1's "no prose restates a limit" rule
was false at the moment it was made; `maxAnchorGap` and `maxLocusDigits` were
added to `Options` so the two bounds with no home got one. Three invariants
named fixtures that could not produce their assertions.

**Loop 8** — **C0 · H2 · M6**. Loop 7's fixes proved to hold unprompted. Both
HIGHs came from one root cause: **"bare" named two different shapes** — a bare
*basename* and a bare *continuation* — and ladder steps 4 and 5 turn on exactly
that distinction. § 2.4 now names three shapes. Method note: § 2.3–2.5 was
accepted clean at breadth in loop 7 and never given a depth read; loop 8's
breadth lane opened a real defect there. Separately, one breadth lane filed five
findings of which three were false — it made two tool calls and reported the
document "cut off" partway. **A lane that silently truncates its read is worth no
more trust when it says CLEAN than when it says CRITICAL.**

**Loop 9** — **C0 · H2 · M7**. § 2.3–2.5 got a depth lane on loop 8's lesson, and
returned every finding here bar one. Its top finding was a defect **loop 8
introduced**: a parenthetical added to ladder step 4 admitted a shape that can
never arrive there. The second was a verified misstatement about existing code:
the `basenameIndex` walk filters twice, on roots *and* on
`CodebaseIndex::isIndexableSuffix`, so a `spec.md:12` is invisible to step 2 even
under `src/` — 180 of 1,383 bare basenames, 13%. **Two lanes' findings had to be
discarded wholesale**, each naming values as undefined which are plainly in the
`Options` block, on nine and five tool calls against a ~1,900-line document — the
first sign that the spec had outgrown what a cheap lane can hold.

**Loop 10** — **C0 · H9 · M21**. The count went *up*, and the reason is the
method: this loop dropped the cheap breadth tier entirely and ran three strong
lanes, after three separate cheap lanes had filed confident findings about text
they never read. Treat loops 8–9's tallies as a floor. **Four of the nine HIGHs
were introduced by loops 7–9's own fixes** — INV-40 rewritten to say an overflow
assertion "would prove nothing" while § 4 still ordered one; INV-35's repair
incomplete, because at the default `max_range_lines` all three citations fit;
loop 8's cache-charge rule unimplementable, needing a file's line count before
reading it; loop 9's gate-G sentence false for a `missing_file` antecedent. A
fifth was a **loop 7 finding dismissed as dated drift**: scan P is 1,934, and
loop 9's edit wrote the derived 1,383 into § 2.4 while § 1 kept 1,384 — a live
contradiction inside a spec for a verb whose purpose is catching that. Every § 1
figure was re-measured. Structurally, `spec_log op:append_loop` had created a
**second** loop-log heading; merged by hand and filed as ANTS-3651, companion to
ANTS-3650.

**Loop 11 (2026-07-27)** — **C0 · H9 · M22 · L/I ~25**, three strong lanes, no
breadth tier, every HIGH verified against source. Two claims were dismissed:
that § 1's ANTS-1750 figures do not reproduce (they do — 56 / 10 / 7; only the
recorded command was wrong, and is now corrected), and that the SSOT rule is
over-broad because `kMaxIndexFiles` is named without an `Options` home (it is
`codebase_index`'s bound, not this verb's; the rule now says so).

*The wiring section carried a symbol that does not exist and a mechanism that
runs backwards.* `processTools` is named in two stale comments and nowhere else;
the real `tools/list` description loop prepends `[<kind>] ` **only when the
description does not already start with `[`**, so a hand-written bracket costs
the verb its kind tag rather than doubling it. Ten loops read past both.
Alongside it, `docCitationsValidate` was specified as the enforcer of
`maxDocBytes` while taking no `Options` — an implementer would have had to
invent the literal that § 2.1 forbids inventing. Both are now pinned, and INV-18
says how the refusal is actually tested.

*Five of the nine HIGHs were fix-induced, and two MEDIUMs were induced by this
loop's own carry-over edits, hours old.* Loop 10 fixed the "uncovered bare
basenames resolve nowhere" claim in § 6 and left § 1 stating the old version.
Loop 10's INV-26 repair still left four of six status buckets empty, which drags
INV-41's "and for no other status" half with it. A limit's `Options` comment
stated its trigger condition twice in four lines. § 5's bytes-per-line "worst
case" had been re-broken after loop 7 fixed it — the corpus's worst is
`mcp-error-codes.md` at 179, not `ROADMAP.md`'s 96. And § 5's peak table, framed
as "every buffer alive at once", had no row for the doc's own raw bytes: the
peak is ~63 MiB, not ~55.

*So this loop stopped patching and cut.* Lane 3, reading cold, filed the
document's length as a finding against `specs.md` § 5.3 — 2,097 lines against
ANTS-3601's 552 for comparable surface and against this spec's own ~470–520-line
implementation estimate, where the standard's own words are that an over-long
spec *"actively delays its own convergence."* That is measurably what had been
happening: three lanes independently filed the same shape of dedup finding — the
20%-coverage arithmetic stated three times, the read-budget rule twice, the
soft-`max_bytes` rule twice, the ANTS-3649 interface three times, two sentences
duplicated verbatim inside a single passage. Reconciling those copies is what
loops 8–11 each did, and it is why each loop found the next set. So the fix this
time was deletion: one home per fact, a pointer everywhere else. **The fixes and
the cut landed in one pass**, because most of the findings *were* duplicate
copies that had drifted apart, and deleting the copy is both the shorter fix and
the one that cannot recur.

*The cut fell short of its target, and the target was wrong.* It aimed at
~1,000 lines and reached **1,884** from 2,097. Every duplicate copy the lanes
named is gone; what remains is contract, and it does not compress much further.
Counted honestly: 47 invariants with verifiable test clauses are ~300 lines, the
`Options` block and six registration hooks ~250, § 2.1 with its worked fixture
~300, the grammar ~135, the ladder ~130 — about 1,100 before § 1, §§ 2.3/2.5/2.6,
§§ 4–7 and this log. So ~1,000 was never reachable at this surface area, and a
future loop should not retry it by trimming prose. The two real levers left are
**moving surface out** (the way ANTS-3649 took the `MarkdownScan` hoist and
ANTS-3642/3643 took `expect` and sweeps) and **reducing the invariant count by
merging contracts that always hold together** — both change what is specified,
not how it is worded, and both need a decision rather than an edit.

**Loop 12 (2026-07-27)** — **C2 · H5 · M14 · L14 · I2**, 35 verified / 0
dismissed / 1 surfaced. Three strong lanes again, no breadth tier. The cut held:
not one finding was a re-divergence of a copy loop 11 deleted, which is the
result that separates "deleted the duplicate" from "reconciled it".

*Both CRITICALs were fixtures that cannot be written as specified.* INV-4
demanded a sort assertion over `tests/Z.cpp` and `tests/_a.cpp` — two paths that
can never meet in one `candidates[]`, because that array holds the paths sharing
**one** basename. Six loops read past a fixture whose two halves contradict the
array they populate. INV-22 asserted that the `tools/list` description does not
begin with `[`, which is false of every emitted description: the loop prepends
`[<kind>] ` to any that lacks it (`claudeintegration.cpp:10256` — the same
mechanism loop 11 corrected one section away, in § 2.7 hook 2, without
propagating it to the invariant that tests it). Two related repairs fell out:
the rule constrains the **authored** descriptor, and `mcp_tools_list_schema` —
which § 4 called "the live `tools/list` schema assert" — is a source scrape of
`claudeintegration.cpp`.

*The five HIGHs split three fix-induced, two never-checked.* `next_offset` was
specified twice over: § 2.1 emits it "exactly when a resumable cap dropped
trailing entries", INV-31 says `max_doc_lines` "emits **no** `next_offset`" —
and when both bind, the literal reading of INV-31 strands the citations that
*were* found, which is the silent shortening § 2.1 forbids by name. INV-17
required three cap fixtures each "distinguishable from the other two using only
response fields", against a § 2.1 table that gives two of them one row on
purpose. INV-13 said the anchor is the "nearest preceding" code span where § 2.5
says **immediately** preceding and spells out the difference two lines later —
§ 2.5's own opening line said "nearest" too, so the contradiction was inside one
bullet. And INV-22 asserted four of nine schema properties: under
`additionalProperties:false` the five it omitted would be client-rejected,
leaving INV-21's entire clamp path unreachable while every listed assertion
passed.

*§ 5's peak table had a row that was not a bound.* The in-flight read was
charged "≤ cache headroom" — nil when the cache is full, while § 2.7 has the
abandoned stream retain "the cited range", and a `:1-9999999` citation's range
is the whole file. The two sentences together permit a whole `maxTargetBytes`
target off-budget. Resolved by naming what the stream actually has to hold: the
lines the response will emit, with § 2.5's full-range anchor search evaluated as
a predicate per line, so retention is `maxRangeLines × maxTextBytes` ≈ 40 KiB
and the ~63 MiB peak stands.

*Four rules were specified and untested, and one was required by a standard.*
`mcp-tools.md` § Tests names three minima for a `Required` verb; `bad_path` and
the happy path were held, `caller_cwd_required` was held by nothing — INV-18
excludes it correctly (the dispatcher answers first), and no other invariant
picked it up, so it is now **INV-48**. Step 2's ladder else, `raw_clipped`'s set
side, the abandonment rule's "already-accumulated lines are kept", and the
engine-raised `read_failed` for an undecodable doc were each folded into an
existing invariant's fixture rather than given one of their own.

*Surfaced, not fixed:* § 2.7's five-step basename-map build calls its own step 4
"not optional and easy to omit … reports every health field green", and nothing
tests it. Covering it needs either a third pure static or a live-MainWindow
test — a change to § 2.7's file table, so it is the author's call, not a
wording repair.

**Loop 12 was the last (2026-07-27).** The loop was stopped and the spec split
instead. It was not converging and the mechanism says it could not: measured
across every commit, this document went 413 lines at draft to 2,095 at loop 12
while the finding count did not fall, because each finding was repaired by
*adding* prose and the additions restated facts that then drifted from their
originals. Loop 11 deleted 200 lines; loop 12 put 198 back. Three of loop 12's
five HIGHs were fix-induced contradictions between two copies of one fact —
the loop's output had become its own next input.

The second signal was sharper. Both loop-12 CRITICALs were **fixtures that
cannot be written as specified**. Twelve careful reading passes missed both;
one attempt to type either fixture would have caught it immediately. For a spec
of unimplemented code there is nothing to check the document *against*, so
review collapses into internal-consistency policing — the one metric that gets
worse as the document grows. The validator is the test, not another reviewer.

What replaced it: the split to ANTS-3653 (§ 2.2 / § 2.3, twelve invariants) and
ANTS-3654 (§ 2.5, two invariants), leaving 1,840 lines here. Four facts that
had been living in two layers were reconciled in the same pass — the sticky
rule moved to § 2.4, INV-1/29/32 stopped asserting resolve-layer facts a
filesystem-free fixture cannot reach, INV-36/40's trap rows followed them out,
and both children now reference every `Options` limit by name with no value
restated. Next is the ANTS-3653 fixtures, not loop 13.

**Loop 13 (2026-09-14)** — **Q1 2 · Q2 1 · Q3 0 · Q4 1**, three lanes, 4
verified / 2 dismissed, 3 fixed. Gated the ANTS-4918 amendment (baseline
`52a78612`); 3 of the 4 verified findings sit on the amended text. Fixed:
INV-49's fixture now makes the date rule, the whole-cell loop-number rule and
"whatever its status" each fail a different row; § 2.4 records step 1b
(ANTS-4381), which the ladder omitted and the amendment's own row 1 repeated;
and a continuation after a `foreign_path` antecedent now inherits that status
(INV-50, the user's decision), where it had re-derived `missing_file` into the
stale list. Deferred: the quotation mode's request arguments have no spec
(ANTS-5216). Dismissed: the Status header, a dated record; and INV-49's
document-wide row scope, which it already states by naming the quotation
check's row test. **Stopped, not converged: loop 2 was skipped at the user's
request to save tokens**, so the build and its red-first tests are the second
check.

**Loop 14 (2026-09-14)** — **Q1 0 · Q2 2 · Q3 0 · Q4 1**, one lane (the user's
choice, to save tokens), 3 verified / 0 dismissed, 3 fixed. Gated the ANTS-4923
amendment (baseline `21673f18`); all 3 verified findings sit on the amended
text. Fixed: § 2.4's "nothing else resets it" left out the `doc-examples` reset
INV-8 is amended with; INV-51's test gained a spaces-only row and a `#tag` row,
so an untrimmed check or an any-`#` heading test fails; and INV-51 now says only
lines strictly between antecedent and continuation count, which two passages had
left open (found resolving a lane question). A second lane question, whether a
shipped fixture crossed a break, resolved clean: the full suite passed after the
build. **One pass, not converged, by the user's choice.**

**impl (2026-11-09)** — not a review loop; no reviewer was dispatched. The
ANTS-5216 amendment: § 2.1's request block and § 2.7's `Options` block gain
`quotes`, `quote_min_chars` and `paths`; a paragraph in § 2.1 names the two
opt-in passes and their items; INV-22 lists the properties its test already
asserts.

<!-- doc-examples: end -->
