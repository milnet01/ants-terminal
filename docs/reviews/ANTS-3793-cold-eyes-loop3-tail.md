# ANTS-3793 — cold-eyes loop 3 (cap): verified findings, unfixed

> **FULLY DISCHARGED (2026-08-04) — this file is now a historical record.**
> Every finding below has been resolved in one of the four specs the umbrella
> was split into. **Nothing here is open.** Read it for provenance, not as a
> work list.
>
> | Findings | Landed in | State |
> |---|---|---|
> | C1, C2, H2, H4, H5, M1–M5, M7, M8, the read-seam LOWs | `docs/specs/ANTS-3793-roadmap-consumer-cutover.md` (rewritten as the read seam) | resolved, accepted |
> | C3, H1 | `docs/specs/ANTS-3808-item-body-and-trailer-suppression.md` | resolved, accepted |
> | H3, M6 | `docs/specs/ANTS-3809-roadmap-write-half.md` | resolved, accepted |
>
> One filed LOW was verified **wrong** and dismissed rather than fixed: the
> claim that stripping `"- "` and the status emoji leaves `body` with a leading
> space. `stripInlineEmoji()` ends by consuming that whitespace, so the parser
> strips it too.

**Status (original):** converged-by-cap at 3 loops (2026-08-03). Loop 3's
contained factual corrections are folded and committed; the findings below were
**verified and deliberately unfixed** because each needed a design decision, not
an edit.

> These were verified and unfixed *at the time of writing*. Do **not** re-review
> to rediscover them — and see the discharge table above before treating any of
> them as work.

**The recommendation that comes with this tail: SPLIT the spec before fixing
it.** Reasoning is at the foot, and it is the reason the run stopped rather than
looping a fourth time.

Two lanes, cold, genre pinned `spec`, shared byte-stable packet
(`shared-context.md`, 35.6 KB). Lane A ~46k input tokens, lane B ~34k — both
inside the 60k budget. Both lanes independently led on the **same** CRITICAL
(INV-2 vs § 2.1's membership rules), which is the signal that makes it credible.

## Already fixed in loop 3 — do not re-raise

| Finding | Fix |
|---|---|
| § 2.1 `status` copied "verbatim" | store holds a lifecycle **word**, `BulletRecord::status` holds an **emoji**; now mapped via `emojiFor()`'s table. Verbatim copy would have failed INV-2 on every record |
| § 2.4 `bundle_row` payload "is the rendered row" | ANTS-3756 § 2.3 + INV-24: `kind='table'` payload is **canonical JSON**; restated as a read-modify-write of one table element |
| INV-5 "two exemptions", "any fourth site is a failure" | a fourth site exists — `rxCommitSha()` (`src/remotecontrol.cpp:22382`) embeds `\bSource:\s*`. Table now carries four sites in three files |
| § 2.5 "one `readProject()` the dialog already needs" | it is `readProjectByRoot()`, and `ProjectRow::legendText` is raw text the dialog must parse |

## CRITICAL — each needs a decision

**C1 — INV-2 is unsatisfiable against § 2.1's membership rules.** *(Both lanes,
independently. The strongest finding in the run.)*
§ 2.1 says the store path returns **every** item including `internal`, `dropped`
and unfiled ones, and that "the render's membership rules do NOT apply here".
INV-2 says the two backends agree field-for-field with `firstLine`/`lastLine`
"the only declared difference". After cutover the markdown backend parses the
**rendered** file, from which ANTS-3758 § 2.4 removes `internal` and `dropped`
and on which its INV-4 **refuses entirely** for an unfiled item. So on any
project holding one of those, the backends provably cannot agree, and
`Inv2BackendsAgree` written as a total comparison fails on real data while
looking like a store bug.
**This is loop-2 collateral:** retargeting INV-2 at the rendered text (loop 2's
C1 fix) was correct in isolation and was never reconciled with the membership
paragraph, which loop 1 wrote.
**Decision needed:** scope INV-2's comparison set to renderable items and add
membership as a second declared difference — *or* filter the store path, which
silently changes `roadmap_query` and is almost certainly wrong. Recommend the
former.

**C2 — `bulletsFromStore()` cannot reach the renderer its own contract is
defined in terms of.**
§ 2.1 defines the store path's `body` as "the rendered bullet minus its leading
`"- "` and status emoji". `renderBullet()` is a free function in an **anonymous
namespace** in `src/roadmaprender.cpp` (§ 2.3 says so, and sells it as "adds no
header surface"), while `roadmapsource.cpp` is a different TU in a different
library. The implementer's only options are a second renderer — which INV-5
forbids in spirit — or an undeclared header change.
**Also loop-2 collateral**, from the same C1 fix.
**Decision needed:** declare the export (e.g.
`RoadmapRender::bulletText(const RoadmapStore::ItemWrite &)`), drop the
"adds no header surface" claim, and fold in **B-H1 below in the same edit**.

**C3 — `body_shadowed` is unimplementable through the declared `TrailerValues`.**
§ 2.4 refuses a column write when "the body's match is **un-anchored**", naming
the shadowing sentence. `TrailerValues` carries five values and nothing else —
no offset, no anchored flag, no matched span. The rule cannot be implemented
through the API § 2.3 declares, so the implementer invents a second matcher
(violating INV-5) or silently drops the refusal, which leaves INV-6's third
fixture unsatisfiable.
**Decision needed:** extend the struct with per-key match provenance
(`struct TrailerMatch { QString value; int offset = -1; bool anchored = false; }`)
or declare a companion accessor, and state that `body_shadowed`'s message is
built from it.

## HIGH

**H1 — INV-6 ships red for the three `^`-anchored keys.** § 2.3 correctly notes
`rxKind`/`rxLayman`/`rxEvidence` are `^`-anchored **with `MultilineOption`**, so
a continuation line *beginning* `Kind:` in prose still matches — but § 2.4's
refusal is scoped to the un-anchored pair (`source`, `lanes`). INV-6 asserts
agreement over all five keys with no exclusion clause, so a body whose
continuation line starts `Kind:` plus a column write is a live counterexample.
**Fix:** either widen `body_shadowed` to all five (the anchored case is
detectable identically) or add an explicit exclusion to INV-6 and file the
residue. *(Partly loop-2 collateral — loop 2 added the MultilineOption caveat
without widening the refusal it undercuts.)*

**H2 — no declared store surface produces document order or a batched read.**
`listItems()` returns `ItemRef{itemPk, idFold, headline, sectionId,
idFromMigration}` (`src/roadmapstore.h:298`) — no status, kind, body, and no
ordering. Document order lives in `listSections()` + `listElements()`, and every
remaining field needs a `readItem()` per item (1,832 on this project). § 4 pins
p95 < 50 ms without naming the query shape that must meet it, and INV-2's
*Breaks when* names the wrong answer ("orders by `id`") without giving the right
one. **Structural draft defect — present since the draft, missed by two cold
reads.** **Fix:** state the derivation (`listSections()` → `listElements()` →
`readItem()`), and either declare a batched reader in § 7's surface additions or
show that N+1 point lookups fit the budget.

**H3 — `id_strategy: "stable_prefix"` is ignored by § 2.4's allocation rule.**
That is a live `roadmap_log` argument under which `append`/`append_batch`
allocate no counter id at all and take `stable_id` verbatim. Routing all `append`
allocation through `idHighWater()`/`corpusHighWater()` breaks every stable-id
project (Sh4, Ts20-SP6, …). **Fix:** scope the store allocation to
`id_strategy: "counter"` and state that `stable_prefix` is unchanged by cutover.

**H4 — the migrated path still loads the whole roadmap, and § 4 does not price
it.** § 2.1 makes `markdown` REQUIRED so § 2.2's ants-v1 gate can run off the
live file. So **no consumer stops reading `ROADMAP.md`** — 2.9 MiB here, on every
call, on the migrated path too. § 4's RAM comparison presents the store read as
replacing that cost when it is additive, and INV-9's *Breaks when* forbids only
*the resolver* re-reading the file while the caller is required to. **Fix:** price
the retained markdown load into § 4's RAM and p95 budgets, reword INV-9's break
clause, and name § 7's owed source-format column as the removal path.

**H5 — `body`'s continuation indentation is unpinned and INV-2 fails on every
multi-line bullet.** § 2.1 says "the rendered bullet minus its leading `"- "` and
status emoji"; `renderBullet()` writes continuations through `appendIndented()`,
which prefixes two spaces, while `parseBullets()` appends each continuation
**trimmed of indentation**. Taken literally the two backends' `body` differ by
that indent. **Fix:** "…and each continuation line trimmed of its indentation,
i.e. exactly what `parseBullets()` would build from that text". **Fold into C2.**

## MEDIUM

- **M1** — § 2.2's two store-state outcomes ("not existing" vs "failing to
  open") are decided by whoever calls `open()`, which the spec never names;
  `RoadmapStore` separates construction from `open()`/`isOpen()`. State that the
  resolver owns the lazy open and stats the path first.
- **M2** — a migrated project whose `ROADMAP.md` is absent, empty or mangled
  yields `sawSignal == false`, so § 2.2 returns `nullopt` and serves markdown to
  a migrated project — the exact silent fallback INV-3 forbids. On a project
  *with* a store row, a failed ants-v1 detection should be `*error`-set.
- **M3** — `bulletsFor()`'s own three outcomes are never stated; only
  `migratedProject()`'s are. An implementer may return `nullopt` for "unmigrated,
  caller should parse".
- **M4** — INV-9's 16 MiB ceiling has no measurable pre-read trigger; resident
  size is knowable only after materialising, and `too_large`'s taxonomy entry
  describes a resource statted **before** it is read into RAM. Express it in a
  cheap proxy (item count, or summed text-column `length()`).
- **M5** — `Inv9Budgets`' fixture is sized at "~2× this project's bullet count",
  which by § 4's own arithmetic lands at or **below** 16 MiB, so the refusal may
  never fire. Size the fixture from the ceiling, not from a corpus multiple.
- **M6** — `idHighWater()` is also `std::optional<qint64>` and its `nullopt`
  case is unstated; `append_batch`'s contiguous `first_id+i` allocation and every
  op's `dry_run` mode (must not commit under mutate-then-render) are uncovered by
  a section titled "roadmap_log's eight ops".
- **M7** — `boldId` "always empty" is scoped to the bracket head, but
  `renderBullet()` emits the bracket only `if (!it.id.isEmpty())`; an id-less
  item's head starts `**<headline>**`, and a single id-shaped headline word
  satisfies `rxIdShaped`. Scope the claim to items with a non-empty `id`.
- **M8** — unfiled items are returned with `sectionId == 0` and therefore have no
  document position, but "Document order" is promised unconditionally. State
  where they sort. *(Interacts with C1 — resolve together.)*

## LOW

- INV-5's prose said "two enumerated exemptions" over a table of three — fixed in
  loop 3, noted here only because the count and the table must stay in step.
- § 4's "users experience per keystroke in the dialog" contradicts § 2.5's "on
  dialog close and reopen, not per scroll event". Name which read is per-keystroke.
- § 2.1's illustrated `body` result drops a leading space: `renderBullet()` builds
  `"- " + emojiFor(status)` then `" [" + id`, so stripping `"- "` and the emoji
  leaves a leading space. Pin it or defer to the parser's own whitespace handling.
- § 2.2's signature comment uses `engaged` for the `has_value()` case, a term
  used nowhere else in the document.
- `bulletsFromStore` / `migratedProject` / `bulletsFor` take `QString *error`
  with no default while `readProjectByRoot()` uses `QString *error = nullptr`.
- § 2.3's "So the headline is emitted twice…" is stranded at the end of the
  anonymous-namespace paragraph, two topics from the claim it concludes.
- § 2.2's "opened lazily on the first migrated read" conflicts with the dispatch
  needing an open store to *discover* migration.

## Open questions the lanes could not settle

1. Can a migrated project actually hold `internal` / `dropped` / unfiled items at
   query time, or does the migration guarantee none? The answer decides whether
   C1 is a scope fix or a design change. (`ItemRef::sectionId`'s comment says
   `0` is "transiently, mid-rebuild", which hints at *none* — unverified.)
2. Is § 2.1's field table actually total over `BulletRecord`? The claim "Every
   `BulletRecord` field is accounted for" was not checkable from the packet.
3. Does any declared reader expose the per-project legend § 2.5 requires?

## Why this run stopped at the cap instead of looping again

Both of Phase 5's stop-and-consolidate triggers fired:

- **Collateral outnumbers draft defects, two loops running.** Loop 2 reported
  both its CRITICALs as loop-1 collateral. Loop 3 is ~7 collateral (C1, C2, C3,
  H1, H4, H5 and INV-5's fourth site all trace to loop-2's own fixes) against
  ~5 draft defects. The fixes are generating defects faster than the reads
  remove them, and another loop pays a full cold dispatch to find damage a
  harder blast-radius sweep would catch for the price of a grep.
- **New *structural* draft defects at loop 3.** H2 (no store surface produces
  document order or a batched read) and the `bundle_row` payload model were
  present since the draft and were reached by neither of two prior cold reads —
  evidence that the document is too large to be read thoroughly end to end, not
  that a third read was needed.

**The document is 934 lines and carries seven contracts:** a reader seam, a
dispatch marker, the ANTS-3808 body/render fix, a write half over eight ops, the
dialog + legend, a round-trip oracle, and an acyclicity check. Splitting before
fixing is cheap now; at loop 8 it is eight wasted loops. Suggested cut, each
independently reviewable:

1. **The read seam** — § 2.1, § 2.2, INV-2/INV-3/INV-9 (+ the C1/C2/H2 decisions).
2. **ANTS-3808** — § 2.3, INV-1/INV-5/INV-6 (+ C3/H1). Already has its own id.
3. **The write half** — § 2.4, INV-4 (+ H3/M6).
4. **The oracle + acyclicity** — § 2.6, § 2.7, INV-7/INV-8. Nearly free-standing.

§ 2.5 (dialog) rides with (1). That is a recommendation, not a decision — the
user's call.
