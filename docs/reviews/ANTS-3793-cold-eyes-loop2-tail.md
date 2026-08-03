# ANTS-3793 — cold-eyes loop 2: verified findings, unfixed

**Status:** run STOPPED mid-loop-2 (2026-08-03), one loop owed. Loop 1 is
folded and committed (`ce302e82`); loop 2's lanes returned and their findings
are recorded below **unfixed**.

> These are verified and unfixed. Do **not** re-review to rediscover them — a
> fresh loop costs a full two-lane dispatch (~295k subagent tokens for loop 2
> alone) to regenerate what is already written here. Fold them in directly,
> then run loop 3 cold.

Subject: [`docs/specs/ANTS-3793-roadmap-consumer-cutover.md`](../specs/ANTS-3793-roadmap-consumer-cutover.md).
Two lanes, cold, genre pinned `spec`, shared byte-stable packet.

## CRITICAL

**C1 — `body` diverges between the two backends, and INV-2 forbids it.**
§ 2.3 stores `item.body` with its first line removed, so the store path's
`BulletRecord::body` has no head line; the markdown path's keeps it (§ 2.1:
`parseBullets` "stays exactly that"). INV-2 declares `firstLine`/`lastLine`
"the only declared difference", so `Inv2BackendsAgree` fails **by
construction**. *This is loop-1 collateral* — § 2.1's field table and INV-2
were both written in loop 1, against a § 2.3 rule that predates them.
**Fix:** decide whether `bulletsFromStore()` re-synthesises `<id> **headline**`
at the front of `body`. If not, add `body` to INV-2's declared-difference list
and say what the equality test compares instead.

**C2 — § 2.1's table over-fills `boldId` / `idToken`.**
The table says "from the item's `id`". VERIFIED against `src/roadmapparse.h`:
"when `boldId` is non-empty, `id == boldId`" — so `boldId` is **empty** for the
ordinary `[ANTS-NNNN] **Headline**` bullet, which is the commonest shape this
backend serves. `idToken` is the leading-slot token *as written*, kept distinct
from `id` for the quarantine case. Filling either from `id` makes INV-2 fail on
the common path — the opposite of loop 1's intent, which was to stop them being
wrongly left empty. **Fix:** state the derivation per bullet shape, or add a
stored column, and list any residual as a declared INV-2 difference.

## HIGH

**H1 — INV-5 is red against today's tree.** VERIFIED: `rxBoldLayman` at
`src/remotecontrol.cpp:6576` is a second trailer-key regex outside the one
exempt file (`ANTS-1933`, `cmdChangelogLog`). An implementer either deletes a
working path or invents exemptions the spec does not authorise. **Fix:**
enumerate the exemptions, or require that call site to move to
`trailerValuesIn()`.

**H2 — `trailerValuesIn()` has no normalisation contract, so § 2.3's fix may be
inert.** § 2.3's "equals the column by construction" holds only if the accessor
reproduces `parseBullets()`'s *post-match* work: `rxTrailerKey` truncation, the
trailing-period chop, `lanes` splitting on commas, `layman`'s punctuation-free
rule. Raw captures would never compare equal, suppression would never fire, and
ANTS-3808 would not actually be fixed. **Fix:** require the accessor to return
values *as `parseBullets()` assigns them*, and add that equality to INV-6.

**H3 — the ants-v1 gate has no input.** § 2.2 requires
`detectRoadmapFormat(lines, &sawSignal)` read "off the live file", but
`migratedProject(store, projectRoot, error)` takes no lines or path, and
`bulletsFor()`'s `markdown` is documented "used only on the unmigrated path".
Compounding: § 2.2 resolves per call, so this is a per-call re-read of a 2.9 MiB
file against § 4's p95 < 50 ms. **Fix:** put the text or path in the signature;
say whether the probe reads the whole file or a detection prefix.

**H4 — § 2.4 contradicts `roadmap-format.md` § 3.5.1.** The spec says that
standard "defines the counter as the sole source"; the standard says the counter
is "a derived, per-machine cache — NOT source (ANTS-3450)" whose allocations
*floor* to the committed corpus high-water. Swapping in `idHighWater()` alone
drops that floor. **Fix:** restate § 2.4 against the standard's actual text and
say whether the corpus floor survives on the store path. *(Lane quotation not
independently re-verified before the stop — check the standard first.)*

**H5 — § 1's "26 call sites … untouched" is wrong by § 2.1's own signature.**
`bulletsFor()` has a different signature from `parseBullets()`, so every
consumer site must be edited. **Fix:** reword to "each swaps one call for the
resolver; no site grows backend logic", or make `RoadmapDialog::parseBullets()`
itself the resolver and give the forwarder its new signature.

## MEDIUM

- **M1** — an unfiled item is readable (§ 2.1) but ANTS-3758 INV-4 **refuses**
  the whole render on one, so a project holding one is permanently unwritable
  under § 2.4's mutate-then-render. Unreconciled.
- **M2** — nobody canonicalises `projectRoot` before `readProjectByRoot()`; a
  mismatch reads as unmigrated, which is the silent fallback INV-3 forbids.
- **M3** — `SourceRow` does not exist; it is `RoadmapMigrate::Source`
  (`src/roadmapmigrate.h`).
- **M4** — § 4 says `ants_roadmapstore_lib` is "linked only by the
  `ants-terminal` executable"; `CMakeLists.txt:2128` sits in the test-bundle
  region, so at least one non-executable target links it. Re-measure; the
  layering argument survives either way.
- **M5** — § 6 says the ANTS-3806 fixture "does contain the `DEMO-0003` bullet
  § 2.3 quotes", but § 2.3 quotes no bullet. Drop the clause or quote it.
- **M6** — the 16 MiB and p95 < 50 ms budgets have no invariant, no case in
  § 6's table and no refusal code. Untestable as written (dim 15).
- **M7** — one code (`bad_op_combo`) is used for two unrelated refusals
  (unsupported locator; shadowed write). Allocate two per
  `docs/standards/mcp-error-codes.md`.
- **M8** — `headline` is assigned in two rows of § 2.1's table with different
  rules; `truncateEllipsis()` appends an ellipsis, which "equal-or-truncated"
  does not pin.
- **M9** — `idHighWater()` takes a prefix the spec never sources, and
  `idPrefixFor()` can return `nullopt`.
- **M10** — store ownership and connection lifetime are unstated, and they are
  the dominant term in the p95 budget. "Marker resolved per call" is not the
  same as "connection opened per call".

## LOW

- § 5's un-anchored-key bullet restates § 2.3 nearly verbatim — reduce to a
  pointer plus the owed-id note.
- `rxKind`/`rxLayman`/`rxEvidence` are `^`-anchored **with `MultilineOption`**,
  so a continuation line *beginning* `Kind:` in prose still matches; § 2.3's
  "cannot be mis-extracted from prose" is narrower than stated.
- § 2.7 (acyclicity) has no relation to a reader seam; only co-location is
  argued. Cohesion.
- § 6 says "an existing bundle" without naming which.
- `findRelationshipCycles()` ships with no scheduled caller — say so.
- INV-1's "never zero, which is ANTS-3758's INV-12" reads as though INV-1
  asserts a zero case; move to rationale.
- § 2.3's "ten such writes" — `grep -c` counts matching *lines*, not calls.

## Open questions the lanes could not settle

1. Does `RoadmapDialog::collectCurrentBullets()` route through `parseBullets()`?
   § 1's out-of-scope list depends on it.
2. Do the card-render paths (`bulletPayload()`, `renderHtml()`, `applyInline()`)
   construct any trailer-key regex? INV-5's exemption list depends on it.
3. Is ANTS-3758 § 5's *scheduling* guard reusable as a *dispatch* gate?

## Dismissed on verification (do not re-raise)

- "No refusal exists for a non-`Bulk` connection" — it does:
  `src/roadmapmigrateload.cpp` refuses `project_refused`. Loop 1 lanes raised
  this because the file was missing from the packet; loop 2's packet includes it.
- "The ANTS-3806 fixture has no `DEMO-0003` bullet" — it does
  (`tests/features/roadmap_migrate_archive_root/test_roadmap_migrate_archive_root.cpp`).
