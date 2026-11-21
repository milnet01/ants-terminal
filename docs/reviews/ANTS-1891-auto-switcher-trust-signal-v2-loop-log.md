# ANTS-1891 — Auto-switcher trust signal v2: inconclusive bucket, harm signal, sample-size floor, session-end positive — review loop log

Record for [`../specs/ANTS-1891-auto-switcher-trust-signal-v2.md`](../specs/ANTS-1891-auto-switcher-trust-signal-v2.md) § 10. Cold-eyes loop log.

## Cold-eyes loop log

_(Filled per `docs/standards/specs.md` § 5.7 as the spec is reviewed.)_

### Loop 1 (2026-05-26) — folded

Folded entries (1 CRITICAL, 3 HIGH, 3 MEDIUM, 1 LOW = 8 entries
below). Reviewer returned 4 MEDIUM total; one (their MEDIUM-1
was a verified-accurate observation about the live ledger, no fix
needed) is omitted from the fold list. Reviewer was correct on
every cited file:line.

- **CRITICAL-1 / `nowMs` plumbing** — § 2.5 only described the path
  overload change; public overload's signature change was implicit.
  Folded: § 2.5 now lists both overload signatures explicitly and
  describes the forwarding chain.
- **HIGH-1 / `changed` flag invariant** — the consequence of omitting
  the 6th comparison (legacy record's clean-end credit lost) was
  prose-only. Folded: new INV-12 makes it a contract.
- **HIGH-2 / `computeOutcome` signature break** — v1 test bundle
  would have failed to compile on the signature edit. Folded:
  `nowMs` defaulted to `0` at the end of the parameter list; § 6
  Migration documents the bit-for-bit pre-1891 behaviour at
  `nowMs == 0`.
- **HIGH-3 / settlement gap** — the central new clean-end branch was
  unreachable for the spec's stated dominant case (0-turn end-of-task
  records never settle under the three pre-existing clauses). Folded:
  § 1 problem statement gains the fifth-issue paragraph; § 2.1 adds
  the new `dwellEndedByQuietWindow` settlement clause; new INV-13
  contracts it.
- **MEDIUM-2 / INV-6 hard-coded "10"** — substring would silently
  break if `kHeadlineFloorMeasured` is bumped (OQ-1 invites this).
  Folded: INV-6 reads the constant symbolically; § 7 test approach
  computes the substring from the constant.
- **MEDIUM-3 / "INV-10" ambiguous self-reference** — § 4 RAM/build
  said "per INV-10" which collides with this spec's own INV-10.
  Folded: now reads "per ANTS-1735 INV-10".
- **MEDIUM-4 / § 5.7 forward reference** — § 10 referenced "§ 5.7"
  without naming the document. Folded: now "per
  `docs/standards/specs.md` § 5.7".
- **LOW-5 / `computeOutcome` line range** — spec cited `L237–296`;
  function actually spans L203–296. Folded: corrected in § 1 (4)
  and § 2.1.

Reviewer also raised one *open question* worth promoting: pre-1891
settled records never get retroactive clean-end credit. Folded as
**OQ-3**.

### Loop 2 (2026-05-26) — folded

Folded entries (0 CRITICAL, 2 HIGH, 3 MEDIUM, 3 LOW = 8 entries
below). Reviewer returned 4 LOW total; one (their LOW-5 was a
verified-accurate observation about test compatibility, no fix
needed) is omitted from the fold list. Reviewer was correct on
every cited file:line.

- **HIGH-1 / bundle wiring** — § 4 / § 7 originally read as if the
  v2 tests would be a *new* top-level bundle exe ("single bundle exe
  linking ~10 cpp files"). Per ANTS-1217 and
  `tests/features/README.md` § Bundles, new tests join an existing
  bundle exe via SOURCES entries — they do not earn a new
  `add_executable`. The existing `model_switch_ledger` and
  `mcp_model_switch_stats` tests live on the `test_core` bundle at
  `CMakeLists.txt` L1507. Folded: § 4 and § 7 now name
  `ants_add_core_bundle(test_core ...)` as the owner and explicitly
  forbid a new `add_executable`.
- **HIGH-2 / header file omission** — § 2.5 described the
  `claudestatuswidgets.cpp` changes but never named the matching
  declarations in `claudestatuswidgets.h` (L109 + L116). Without the
  header edit, INV-13's test cannot compile. Folded: § 2.5 calls
  out both declarations; § 8 Cross-doc lists the header file.
- **MEDIUM-1 / pre-change vs post-change ambiguity in § 2.5** — the
  original prose described the forwarding chain as if it were already
  present in the source; reading L1532 a reviewer would see the
  single-arg call and conclude the change was already partly done.
  Folded: split into **Pre-change** / **Post-change** subsections so
  the implementer reads the delta unambiguously.
- **MEDIUM-2 / `inputBefore` default preservation** — the § 2.1
  fenced block added `nowMs` at the end of the parameter list but
  did not annotate the existing `inputBefore = {}` default. Folded:
  inline `// default preserved` comment + § 2.1 sentence.
- **MEDIUM-3 / tool-descriptor field enumeration** — the
  `model_switch_stats` tool descriptor in `claudeintegration.cpp`
  L2161–2181 enumerates envelope field names verbatim; the four new
  fields must be appended so clients know they exist. Folded: § 8
  Cross-doc impact now names the descriptor.
- **LOW-2 / "~10 cpp files" inaccurate** — the existing
  `model_switch_ledger/` directory contains a single test file, not
  ten. Folded: § 4 rewritten ("13 small TUs totalling ~600 lines").
- **LOW-4 / ANTS-1735 INV range broadened** — § 8 said "INV-10..
  INV-12" but ANTS-1735 also defines INV-13 + INV-14. Folded:
  "INV-1 .. INV-14 all stay verbatim."
- **LOW-6 / constant sharing** — `kCleanEndQuietMs` is the same
  numeric value as `kOverrideCooldownMs`. Folded into **OQ-2** with
  rationale for keeping them separate (different questions, could
  diverge later) and the explicit option to merge if a future tuning
  pass concludes they must move together.

Loop 2 found no Critical findings, no doc-vs-code accuracy regressions
on the loop-1 fixes, and no resurfaced loop-1 findings — the loop-1
fixes held.

### Loop 3 (2026-05-26) — folded

Folded entries (0 CRITICAL, 2 HIGH, 3 MEDIUM, 2 LOW = 7 entries
below). Reviewer returned 4 MEDIUM and 4 LOW total; two were
non-findings (MED-4 a stylistic note on `QStringLiteral` formatting
inside a test description; LOW-2 a vague "future spec" note tagged
INFO; LOW-4 unicode-hyphen consistency) and were not folded. No
loop-1 or loop-2 findings resurfaced — earlier fixes held. New
findings:

- **HIGH-1 / bucketing ambiguity** — INV-2 / § 2.2 didn't say
  explicitly which bucket a clean-end-only (0-turn, no negative
  signal) record lands in. The intended behaviour is "measured AND
  contributes ½ to weighted_avoided," but a reader could read INV-2
  as "either-or." Folded: § 2.2 gains a five-row truth table walking
  the implementer through every settled-downgrade shape.
- **HIGH-2 / § 2.2 fragment risk** — the new code block could be
  mis-read as a *replacement* loop, dropping the existing
  `downgrades`, `opusAvoided`, `by_tier`, `pendingCount` counters.
  Folded: prose now says "amend (do not replace)", names the
  pre-existing counters to preserve, and the fenced block carries
  a trailing comment listing them.
- **MEDIUM-1 / § 2.5 self-contradiction** — "both overloads gain a
  `nowMs` parameter" contradicted the very next paragraph that
  showed the public no-arg overload with "// unchanged signature."
  Folded: the public overload's *external* signature stays put;
  only the path overload's wire signature changes; the public
  body internally computes and forwards `nowMs`.
- **MEDIUM-2 / Outcome field placement** — the new
  `sessionCleanlyEndedOnNewTier` is inserted before `pending` in
  the struct; the placement is deliberate (groups signal bools
  contiguously for JSON serialisation order) but unannotated.
  Folded: inline comment in the fenced block.
- **MEDIUM-3 / `nowMs == 0` unreachability proof** — only covered
  the `lastAssistantTurnTsMs > 0` branch; missed the empty-turns
  fallback to `switchTsMs`. Folded: § 2.1 prose extended to cover
  both branches.
- **LOW-1 / `kCleanEndQuietMs` duplicate declaration** — declared
  in both the § 2.1 fenced block and § 2.4. Folded: declaration
  centralised in § 2.4, § 2.1 block comment references it.
- **LOW-3 / unnamed flip-gate spec** — § 5 Out of scope referenced
  "ANTS-MMMM" without a placeholder. Folded: rewritten as "future
  ROADMAP item (id not yet allocated)" — no number reserved.

Reviewer also raised one *open question* worth promoting: INV-4 with
an entirely empty transcript (no user OR assistant turns since the
switch). Folded by adding test case (e) in § 7's `test_session_clean_end.cpp`
row — the `max(switchTsMs, lastAssistantTurnTsMs)` fallback handles
this case via `switchTsMs`.

Loop 3 confirmed loop-1 and loop-2 fixes held — no resurfaced
findings; structural fixes (bucketing, fragment risk, self-
contradiction) addressed the last remaining ambiguity surfaces.

### Loop 4 (2026-05-26) — folded

Verified findings (0 CRITICAL, 2 HIGH, 3 MEDIUM, 2 LOW). No loop-1,
loop-2, or loop-3 findings resurfaced — earlier fixes all held. New
findings (all verified against current source + live ledger):

- **HIGH-1 / §1 "never settles" overstated** — the prose claimed a
  true end-of-task 0-turn downgrade "stays `pending=true` forever,"
  but the 2026-05-26 live ledger shows BOTH 0-turn records settled
  (first via `dwellEndedByNextAuto`, second via what is most
  plausibly `dwellEndedByDivergence`). Only the *trailing* 0-turn
  record in any project's history is genuinely stuck. Folded: § 1
  ¶2 narrowed to the trailing-record case with the ledger evidence
  spelled out; the motivating "fifth structural issue" claim is
  honest about its scope.
- **HIGH-2 / INV-range inconsistency between §1 and §8** — the spec
  header said "ANTS-1735's INV-10..INV-12 stay unchanged" while
  §8 Cross-doc said "INV-1..INV-14 stay verbatim." Loop 2 LOW-4 had
  fixed §8 but missed §1's mirror sentence. Folded: §1 header now
  matches §8 — "INV-1..INV-14 stay unchanged."
- **MED-1 / JSON field-name convention claim** — the spec asserted
  the new key `session_cleanly_ended_on_new_tier` "matches the
  existing field naming in `toJson`/`fromJson`" — half-true (case
  style matches; the `_within_N_turns` suffix the other three signal
  keys carry deliberately doesn't apply). Folded: § 2.1 spells out
  the suffix divergence and its rationale (quiet-window measurement,
  not a turn-count window).
- **MED-2 / ROADMAP-vs-spec name drift** — `ROADMAP.md:19418` used
  the working name `session_completed_cleanly_on_new_tier`; the
  spec uses `session_cleanly_ended_on_new_tier` (verb swap). Folded:
  ROADMAP.md is renamed in-place in this commit so the two stay
  in sync until promotion; § 2.1 + § 8 acknowledge the rename
  explicitly.
- **MED-3 / divide-by-zero envelope-reader hazard** — INV-3 told the
  implementer `regret_rate=0.0` when `measured_downgrades==0`, but
  didn't warn envelope consumers that this is now reachable with a
  non-empty `recs` list (a ledger of entirely inconclusive records).
  Folded: INV-3 now explicitly tells consumers to gate on
  `measured_downgrades > 0` (or the headline INV-6) before treating
  `regret_rate` as meaningful.
- **LOW-1 / "bundle" phrasing** — the prose called
  `tests/features/model_switch_ledger/` "the existing bundle," but
  it's a SOURCES entry on the `test_core` bundle, not its own
  bundle. Folded: § 2.1 reworded to "the existing `model_switch_ledger`
  test files on the `test_core` bundle."
- **LOW-2 / INV-11 boundary asymmetry unexplained** — the test
  fixture uses `nowMs - kCleanEndQuietMs - 1` (1 ms) and
  `nowMs - kCleanEndQuietMs + 1000` (1 s) either side of the
  boundary; the asymmetry is intentional (1 ms below exercises the
  inclusive `≥` check; 1 s above is well clear of rounding) but
  unannotated. Folded: INV-11 now states the rationale inline.

Reviewer raised one *open question* — that the original OQ-3 read as
a decided design note rather than an actually-open question. Folded:
OQ-3 reclassified as "**Design note**" (no decision pending).

Loop 4 confirmed loops 1–3 fixes all held. No structural surfaces
remain — every finding from loop 4 was either prose precision
(HIGH-1, MED-1, MED-3, LOW-1, LOW-2) or paperwork alignment
(HIGH-2, MED-2, OQ-3 reclass). No new contracts introduced.

### Loop 5 (2026-05-26) — folded

Folded entries (0 CRITICAL, 1 HIGH, 3 MEDIUM, 1 LOW = 5 entries
below). Reviewer returned 2 HIGH; one (their HIGH-1 — "ROADMAP
rename still stale") **failed Phase-3 verification**: the file
`/mnt/Games/Scripts/Linux/Ants_Terminal/ROADMAP.md:19418` was
greppped post-reviewer-return and carried the canonical name
`session_cleanly_ended_on_new_tier` already (the rename had landed
in the loop-4 fold-in). Reviewer's "verified just now" claim was
stale; HIGH-1 dismissed per the cold-eyes Phase-3 protocol. No
loop-1..4 findings resurfaced — earlier fixes all held. New folded
findings:

- **HIGH-2 / INV-12 justification** — the rationale "permanent loss
  of clean-end credit" overstated the loss on the *current*
  production path (the existing `pending` comparison in
  `OutcomeFillResult.changed` already catches the relevant case via
  the in-tick `true→false` flip of `pending`). The sixth comparison
  is genuinely defensive — it protects two forward scenarios (tests
  that call `computeOutcome` on settled records; a future controller
  change that lifts the settled-record skip). Folded: INV-12 prose
  rewritten to mark it **Defensive** and name both forward
  scenarios; the contract still requires the sixth comparison, but
  with honest rationale.
- **MED-1 / loop-log finding-count mismatch** — the headline counts
  in loops 1, 2, and 3 didn't match the listed entry counts (silent
  compression during folding where verified-accurate observations
  and non-findings were dropped from the list but stayed in the
  count). Folded: loop 1, 2, and 3 headlines rewritten to reconcile
  with the actual fold count and call out which reviewer entries
  were omitted (and why) for full audit trail integrity per
  `docs/standards/specs.md` § 5.7.
- **MED-2 / truth-table row 4 jarring read** — the row title
  "Negative + clean-end (turns=0, signal, clean-end=false by INV-4)"
  was internally contradictory until the prose three paragraphs
  later explained the impossibility. Folded: row title renamed to
  "Negative signal, clean-end forced false (INV-4 prohibits both)"
  so the impossibility is readable at a glance.
- **MED-3 / INV-9 "never removed" lacks enforcement** — the
  invariant promised future removal-prevention but the test only
  asserts present-day shape. Folded: INV-9 wording narrowed to "v1
  of this spec" with explicit "removal requires a superseding spec"
  rule.
- **LOW-3 / "No public ABI" overclaim** — the snapshot claim was
  true for today's statically-linked binary but didn't acknowledge
  the architectural option to grow an out-of-tree consumer. Folded:
  § 6 Migration now says "no out-of-tree consumers today" with a
  forward-compat note on the default-arg signature.

Loop 5 confirmed loops 1–4 fixes all held. Both verifications I
performed during fold-in were against current file state, including
the dismissed HIGH-1 (the rename is in place). No fresh structural
surfaces remain.

### Loop 6 (2026-05-26) — folded

Folded entries (0 CRITICAL, 1 HIGH, 2 MEDIUM, 1 LOW = 4 entries
below). Reviewer returned 2 LOW; LOW-1 (headline-form-count text
drift, cosmetic) was a non-finding. No loop-1..5 findings
resurfaced — earlier fixes all held. New folded findings:

- **HIGH-1 / § 2.2 missing local-counter declarations** — the
  replacement-body block uses `++inconclusiveCount` and
  `++cleanEndCount` but the fragment never named them as new
  locals. An implementer following the surgical-edit rule could
  add the `++` lines without the declarations and the build dies
  with two undefined-identifier errors. Folded: § 2.2 prose
  enumerates the two new `int` locals alongside the existing
  counter list at the top of `statsEnvelope` (L299–303), with a
  fenced block showing the declaration form to use.
- **MED-1 / malformed-ts spurious clean-end credit** — `parseIso8601Ms`
  returns `0` on parse failure (`src/modelswitchledger.cpp` L420–
  423). With `switchTsMs=0` and an empty `postSwitchTurns`, the
  quiet-window comparison `nowMs - 0 ≥ kCleanEndQuietMs` succeeds
  for any production `nowMs ≥ kCleanEndQuietMs` (≈10 min after app
  start), spuriously settling the malformed record and firing
  INV-4's positive predicate to credit `+0.5` to `weighted_avoided`.
  Pre-existing settlement clauses don't have this hazard because
  they key off subsequent-record existence. Folded: § 2.1 settlement
  prose and INV-13 contract both gain a `switchTsMs > 0` guard;
  INV-13's test list now includes a malformed-ts case explicitly.
- **MED-2 / INV-11 boundary annotation reversed** — the test fixture
  `lastAssistantTurnTsMs = nowMs - kCleanEndQuietMs - 1` makes the
  delta `kCleanEndQuietMs + 1`, i.e. **1 ms past** the inclusive
  boundary, not 1 ms below it. The implementer reads the prose and
  the arithmetic and gets opposite signals. Folded: INV-11 prose
  now reads "1 ms past the inclusive `≥` boundary" with the delta
  arithmetic spelled out inline.
- **LOW-2 / `cmdModelSwitchStats` line-cite promotion** — § 6
  Migration's "only known caller is the `model_switch_stats` MCP
  dispatch" claim was provable but not cited. Folded: cite added
  inline (`src/remotecontrol.cpp` `cmdModelSwitchStats` at L7553)
  so a future reviewer can re-verify without re-grepping.

Loop 6 confirmed loops 1–5 fixes all held; reviewer explicitly
verified the Loop-5 ROADMAP-rename dismissal stood. The remaining
findings were one missing detail (HIGH-1), one corner case
(MED-1), one precision reversal (MED-2), and one cite promotion
(LOW-2) — no new structural surfaces.

### Loop 7 (2026-05-26) — folded

Folded entries (0 CRITICAL, 1 HIGH, 2 MEDIUM = 3 entries below).
Reviewer returned 2 HIGH, 3 MEDIUM, 3 LOW — two findings were
**dismissed per Phase-3 verification** (both as design-intent
misreads or stale review state):

- **HIGH-1 dismissed** ("quiet-window auto-fires on first launch
  after any 10-min-old pending record") — this is the design's
  *intended* behaviour, not a bug. § 1 problem (d) explicitly
  describes this exact case ("the user finishes the heavy work …
  closes the tab, starts a different project … without further
  turns") as the dominant case the new clean-end signal exists to
  credit. The ½-weight `weighted_avoided` math (§ 2.2) discounts the
  uncertainty that the user might have interrupted rather than
  finished; the trade-off is intentional. Folded only to add an
  explicit "Design intent: empty-`postSwitchTurns` is the credited
  case" paragraph in § 2.1 so a future reviewer doesn't re-raise
  the question.
- **MED-2 dismissed (collapses with HIGH-1)** — INV-4 case (e) of
  the `test_session_clean_end.cpp` test row codifies exactly the
  empty-`postSwitchTurns` clean-end credit. The reviewer suggested
  reconciling with the proposed HIGH-1 fix; since HIGH-1 is
  design-intended, case (e) stays as written.
- **LOW-3 dismissed (Phase-3 stale)** — claim that
  `session_completed_cleanly_on_new_tier` "hasn't landed in ROADMAP
  yet" was re-verified via grep at fold-in time: `ROADMAP.md:19418`
  carries the canonical `session_cleanly_ended_on_new_tier` as of
  the loop-4 fold-in. Same Phase-3-stale pattern as Loop-5's
  dismissed HIGH-1; the reviewer's tool snapshot was older than the
  file state.

Folded findings:

- **HIGH-2 / show post-change body of no-arg overload** — § 2.5
  described the public overload's signature delta but did not show
  the post-change body, leaving the implementer to infer the
  forwarding-call delta. Folded: § 2.5 now includes a fenced block
  with the full post-change body, with a one-line diff narration
  ("the only delta is `, nowMs` appended to the forwarding call").
- **MED-1 / tool-descriptor prose vs field-list** — § 8 Cross-doc
  only said "append the four new fields" but did not call out the
  *behavioural prose* update in the descriptor's `t["description"]`
  (currently "avoided Opus turns vs regret/under-route rate" — now
  subtly redefined). Folded: § 8 bullet split into "(1) field-list
  append" and "(2) prose update" with explicit text covering the
  new headline forms + tightened regret definition.
- **MED-3 / § 1 (a) example wrong on `measured`** — the example
  said "regret_count=0 / measured=1 = 0.0% from one 0-turn record
  with no signals at all," but the current `statsEnvelope` counts
  *both* live-ledger records into `measuredDowngrades` regardless
  of `turnsOnToTier`. Verified against the global stats fetch
  (`switches:2, downgrades:2`). Folded: example now reads
  "regret_count=0 / measured=2 = 0.0% over two 0-turn records (one
  with `under_route_signal=true` but not folded into regret today
  — problem (b) — and one with no signals at all)."

Loop 7 confirmed loops 1–6 fixes all held. Two findings + one LOW
dismissed via Phase-3 verification (design-intent or stale state);
three real findings folded. The remaining open question — whether
the design-intended trailing-record credit is the right call —
is a *product* judgement to revisit only when real-world data
suggests otherwise (Layman's review of `weighted_avoided` over the
next release cycle).

### Loop 8 (2026-05-26) — folded

Folded entries (0 CRITICAL, 1 HIGH, 2 MEDIUM = 3 entries below).
Reviewer's LOW-1 was an INFO-only note (accurate header citation),
no fold needed. No loop-1..7 findings resurfaced — earlier fixes
all held. New findings:

- **HIGH-1 / truth-table "per-signal" shorthand** — the
  `underRouteCount` column of the bucketing truth table used the
  shorthand "per-signal" without spelling out whether it meant
  per-record or per-signal-bool. Folded: a **Note on the
  `underRouteCount` column** paragraph beneath the table now says
  explicitly "1 if `underRouteSignalWithin5==true`, else 0 — gated
  on that single bool, not on the count of negative signals" and
  walks through the user-override+under-route co-occurrence case.
- **MED-1 / CHANGELOG entry severity** — § 8 said "Add a
  `### Changed` entry" but Keep-a-Changelog convention treats a
  same-key semantic shift (regret_count adding under-route) as a
  breaking change worth a prominent callout. Folded: § 8 bullet
  rewritten to mandate "prominently flagged as a behaviour-
  breaking change" with the rationale spelled out.
- **MED-2 / INV-12 surgical-edit anchor** — § 2.1 said "the `changed`
  flag gains a sixth comparison" without showing the existing
  expression OR the diff. Folded: § 2.1 now quotes the pre-change
  `res.changed = …` expression at L290–294 verbatim, then shows
  the post-change form with the appended sixth clause inline —
  same diff style as the § 2.5 body diff.

Loop 8 confirmed loops 1–7 fixes all held. Three real findings
folded; the dispatched reviewer's "Open questions" section was
empty for the first time across the eight loops, and no new
structural surfaces were raised. Approaching a clean pass.

### Loop 9 (2026-05-26) — folded

Folded entries (0 CRITICAL, 2 HIGH, 2 MEDIUM = 4 entries below).
Reviewer returned MED-3 (audit-trail completeness) which dismissed
on Phase-3 verification — every prior loop entry already names its
dropped reviewer findings inline (Loop 1's MEDIUM-1, Loop 2's
LOW-5, Loop 3's MED-4/LOW-2/LOW-4, Loop 4's HIGH-1, Loop 5's
HIGH-1, Loop 6's LOW-1, Loop 7's HIGH-1/MED-2/LOW-3, Loop 8's
LOW-1 — all enumerated where applicable). LOW-1 (file-count desync
risk) was folded as part of MED-1; LOW-2 ("~600 lines" guess)
folded as MED-1. No loop-1..8 findings resurfaced — earlier fixes
all held. New findings:

- **HIGH-1 / `m_lastPendingFillMs` ownership** — § 2.5 didn't call
  out that the throttle state is owned by the no-arg overload only;
  the path overload neither reads nor writes it. A test that
  drives the path overload directly (per the test seam) followed
  by a production call to the no-arg overload would see surprising
  throttle state. Folded: § 2.5 gains a "Throttle ownership note"
  paragraph explicitly documenting this and naming the test
  implication.
- **HIGH-2 / INV-13 silent on user-only post-switch turns** —
  INV-13 said `lastAssistantTurnTsMs` falls back to `switchTsMs`
  when `postSwitchTurns` contains no post-switch assistant entries,
  but didn't say what happens when post-switch *user* turns exist
  without assistant replies. Folded: INV-13 now explicitly
  clarifies that user turns are not consulted for the quiet-window
  check, so a post-switch user turn with no assistant reply yet
  leaves `lastAssistantTurnTsMs == 0` and the predicate reduces to
  the same form as the empty-`postSwitchTurns` case.
- **MED-1 / "~13 TUs" precision** — § 4 RAM/build cost said
  "~13 small TUs" but the exact count is 13 (one per invariant).
  Folded: dropped the `~`, added forward-compat note ("future INVs
  add one TU each"), kept ~600 lines as the line-count estimate.
- **MED-2 / CLAUDE.md constant-name leak** — the proposed CLAUDE.md
  update encoded `kHeadlineFloorMeasured (10)` as a literal value,
  which ties the always-loaded preamble to OQ-1's tunable value.
  Folded: CLAUDE.md update now reads "a configurable floor of
  measured downgrades" — name and value stay in the spec, not the
  preamble.

Loop 9 confirmed loops 1–8 fixes all held. All four real findings
were small precision / clarity polish; no new structural surfaces.
The reviewer's closing note ("Spec is approaching clean. Eight
loops have closed every structural surface; remaining issues are
precision/audit-trail polish.") matches the orchestrator's
assessment of diminishing returns.

### Loop 10 (2026-05-26) — folded

Folded entries (0 CRITICAL, 1 HIGH, 2 MEDIUM = 3 entries below).
No loop-1..9 findings resurfaced — earlier fixes all held. New
findings:

- **HIGH-1 / §2.5 throttle paragraph conflicts with existing
  ANTS-1890 path-overload docstring** — the existing path overload
  carries an ANTS-1890 docstring at `claudestatuswidgets.cpp` L1535–
  1537 saying "bypasses the 30 s throttle"; the new ANTS-1891
  paragraph framed the throttle question as if the path overload's
  contract were ambiguous. Folded: rewritten to **re-state** the
  existing ANTS-1890 contract first ("path overload already
  bypasses the throttle today"), then explicitly note the new
  `nowMs` parameter is **purely a clock seam** for `computeOutcome`,
  not a throttle-state input — so the existing throttle behaviour
  is preserved.
- **MED-1 / stale L111–115 doc comment in `.h`** — the existing
  doc comment on the path overload at `claudestatuswidgets.h`
  L111–115 claims "Throttle bypassed when path != defaultLedgerPath()",
  but the body unconditionally bypasses the throttle regardless of
  path. Since the same commit touches the path overload's
  declaration anyway (HIGH-2 from loop 2), it should also fix the
  stale doc comment. Folded: § 8 Cross-doc's `.h` bullet now
  mandates rewriting the comment to the accurate ANTS-1890
  contract plus the ANTS-1891 clock-seam rationale.
- **MED-2 / `lastAssistantTurnTsMs` derivation not shown** — INV-13
  used `lastAssistantTurnTsMs` as a contract input without telling
  the implementer how to derive it. The existing `onTier` loop at
  L228–238 looks like the obvious reuse target but is **wrong** —
  it early-breaks at the first divergent assistant, so it tracks
  the last *on-tier* turn, not the last *assistant* turn overall.
  Folded: § 2.1 now opens with a "Derivation of
  `lastAssistantTurnTsMs`" subsection containing a 4-line fenced
  block showing the separate scan, with an explicit "do NOT fold
  into the `onTier` loop above" warning.

Loop 10 confirmed loops 1–9 fixes all held. All three findings
were precision additions to the post-fold implementer guidance —
no contract changes, no new invariants. The reviewer's closing
note ("Spec is near-final after 9 cold-eyes loops; the remaining
surface is precision around the controller-side scan, not new
structural concerns.") matches the orchestrator's assessment.
