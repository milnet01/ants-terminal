# ANTS-3757 cold-eyes — run state after loop 2

**Stopped:** 2026-07-31, after loop 2, deliberately — not at the `--max-loops`
cap and not on infrastructure. See *Why stopped* below.

**These findings are verified and unfixed. Do NOT re-review to rediscover them
— a fresh loop costs a full multi-agent dispatch to regenerate what is already
written here. Fold them in directly.**

## Trend — the reason this is a stop and not a loop 3

| Loop | C | H | M | L | Total |
|---|---|---|---|---|---|
| 1 | 4 | 8 | 9 | 5 | 26 |
| 2 | 5 | 12 | 11 | ~10 | ~38 |

Findings went **up**, and roughly half of loop 2's are **fix collateral** —
defects loop 1's own fixes introduced, not defects in the draft. `/cold-eyes`
Phase 5 names that pattern explicitly: *"Collateral rising while draft defects
fall means the sweep is under-running, not that the document is bad — and
looping harder makes it worse."*

**The root cause is structural and nameable.** § 2.1 declares three types
(`PlannedItem`, `PlannedElement`, `Note`), § 2's prose describes the same
contract, and § 3's thirteen invariants assert it a third time. Every loop finds
new disagreements *between the three copies* rather than new defects in the
design. That is the "reconcile N copies vs delete N−1" anti-pattern, and another
cold read will keep finding it. The next pass should **consolidate** — let the
type declarations be the single statement of shape and reduce the prose and the
invariants to pointers — before any further review.

## Loop 2 — CRITICAL

1. **§ 2.3 / INV-6 / INV-7 — the reader this spec mandates cannot supply the raw
   Status word.** VERIFIED against `src/roadmapdialog.h`: `BulletRecord::status`
   is an **emoji** (`"✅"|"🚧"|"📋"|"💭"`) and the record carries no raw
   `- **Status**:` value; `parsePassHeadingBullets()` collapses the word at
   parse time. § 2.7's `asserted`/`defaulted` split and `extras.source_status`
   both need the verbatim word, so both are unimplementable through an as-is
   lift. The word survives only inside `BulletRecord::body`, and re-parsing it
   there is the second parser § 2.3 exists to forbid.
   **Fix:** ANTS-3764's brief must widen the record with the raw Status value.
   *This changes a blocker's scope — already annotated onto ANTS-3764.*
2. **INV-11 — "no double-cover" is falsified by the spec's own note design.**
   Every § 2.10 note except `empty_source` carries a line **inside** an item
   span (INV-12 requires one `duplicate_id` note per item, naming its line). The
   union assertion fails on the very fixtures § 6 requires.
   **Fix:** assert coverage over item + element spans only; require noted lines
   to fall *within* a covered span rather than joining the union.
3. **INV-13 contradicts INV-11.** § 2.1 models prose as a `narration` element
   and INV-11 *requires* every non-blank line be carried — so a prose file
   yields elements and `empty_source` never fires. INV-13's second fixture is
   red against a correct implementation.
   **Fix:** trigger `empty_source` on zero **items** with a narration-only
   carve-out, or replace the fixture with one yielding neither.
4. **INV-7's statement and its own mutation describe opposite implementations.**
   Statement says the verbatim **word**; the mutation forbids "the first token
   rather than the whole value" — the word *is* the first token. Only the test
   leg (`done (v3.20.0, …). Adds catalogs for …`) takes the whole-value reading.
   **Fix:** say "the author's verbatim Status **value**" throughout, and state
   whether the leading `*` stripped for *matching* is also stripped for
   *storage*.
5. **§ 2.1 — `PlannedItem` omits most of `ItemWrite` while claiming exactly two
   fields diverge.** `ItemWrite` carries `resolution`, `milestone`, `priority`,
   `visibility`, `created`, `lastModified`, `shipped`; the `item` DDL adds
   `lanes` and `evidence`, both of which `roadmap_log` writes into markdown.
   None appear in `PlannedItem`. § 2.8 says priority/resolution/dates are "left
   empty" — they cannot even be represented, and `Lanes:` / `Evidence:` lines
   land in `body`, satisfying INV-11 while losing structure.
   **Fix:** add the fields, or add a table naming each omitted field and where
   its source text goes.

## Loop 2 — HIGH

6. **§ 2.7's case-fold and `*`-strip are a second mapping over the same input**
   — exactly what § 2.3 forbids and what § 8 uses to reject
   `partial → in-progress`. The same file would yield `shipped` in the store and
   📋 in `RoadmapDialog`. **Fix:** state whether ANTS-3764 moves the fold into
   the shared reader (preferred; § 7 then owes the `RoadmapDialog` /
   `roadmap_log` impact) or whether this is an acknowledged divergence — and add
   an invariant either way. *(both lanes)*
7. **Archives are outside discovery.** `roadmap-format.md` § 3.5.1 makes the
   committed corpus `ROADMAP.md` + `CHANGELOG.md` + `docs/roadmap/*.md`; § 2.2
   sees only the roadmap file, and INV-11 is file-scoped so it cannot see the
   loss. **Fix:** migrate archives, or add an explicit § 5 exclusion saying why
   not.
8. **Nothing in the read half reads the file.** `findRoadmap()` returns a path,
   `planFrom()` takes a string. No owner, no encoding, no failure mode for an
   unreadable file or invalid UTF-8. *(both lanes)*
9. **§ 2.1 promises "§ 7 records the `ItemWrite` addition it owes"; § 7 does
   not.** Introduced by the loop-2 decision edit, which rewrote § 7's
   roadmap-data-model bullet and dropped it.
10. **INV-2's mutation population is corpus-side, but § 6's fixtures are not.**
    The 96 status-marked bullets are a corpus figure; no fixture carries a
    status-marked detail line or a legend line, so dropping the headline half
    changes no fixture count. Same gap for INV-8's `Kind:` fixtures. *(both
    lanes)*
11. **INV-5's named mutations do not redden its stated assertions.** Omitting
    the emoji rows leaves "no plan yields `dropped`" and the pass-word round trip
    both true; INV-6 would catch it instead. **Fix:** add an explicit
    per-source→status assertion table, and state the round trip's direction.
12. **INV-3's assertion measures item-hood; its mutation moves identity.** If
    the prose tokens are unmarked, INV-2's status-marker half rejects them first;
    if marked with bold headlines, they were already items and only their `id`
    changes. **Fix:** specify the fixture bullets as status-marked with bold
    headlines, and assert `id` empty + `idAllocationOwed == true`.
13. **INV-8's mutation names code outside this half** ("requires `Kind:` at
    write time" — writing is ANTS-3765). **Fix:** "`planFrom()` refuses an item
    with no `Kind:`", plus a `Source:`-absent leg.
14. **INV-9's mutation does not diverge.** A counter *read* returns the same
    value twice; only read-and-**increment** makes two calls differ. The test
    also cannot see a stable-content filesystem touch. **Fix:** name the mutation
    as incrementing, and add a filesystem leg.
15. **§ 2.9's `idAllocationOwed` / `closed` contract has no invariant**, nor do
    the `quarantined_id` and `orphan_status_line` codes. A plan that never sets
    them passes all thirteen.
16. **`PlannedElement`'s kind set is too narrow for INV-11.** Section headings,
    the `<!-- ants-roadmap-format: 1 -->` marker and plain paragraphs match no
    kind and no note code, so the coverage assertion cannot pass on a real
    roadmap.

## Loop 2 — MEDIUM / LOW worth keeping

- **The item count drifted again: the survey now prints 3,956**, not the 3,955
  § 2.4 quotes — this session's own ROADMAP appends moved it a second time.
  Strong argument for folding these counters into the survey tool so the figure
  is output rather than prose.
- **INV-1 says discovery loss is "all 144 of its items"**; RetroDB also has 73
  bullet-form items, so the loss is ~217.
- **§ 2.4 cites "§ 2.9" for the report**; the report is § 2.10.
- **§ 2.10's `status_defaulted` gloss says "word"**, excluding § 2.7's
  no-Status-line row and INV-6's clause.
- **§ 2.7 has a dangling "So:"** introducing nothing — an editing artifact from
  the loop-2 decision edit.
- **§ 4's 3× RAM budget ignores `QString`'s UTF-16 doubling** — one copy of a
  2.9 MB ASCII file is already ~5.8 MB before payloads.
- **§ 4 understates ANTS-3764**: the pass reader is a `RoadmapDialog` member,
  not an anonymous-namespace function, so it is dispatcher + three parsers +
  `BulletRecord`.
- **§ 2.5's inline `grep -c`** prints per-file counts, not the single totals its
  comments claim; needs `| awk '{s+=$1} END{print s}'` and a stated cwd.
- **§ 2.4's `python3` snippet** uses `^\s*-\s+` where the shipped detector uses
  `^\s*[-*]\s*`, so the 154 figure may not be the detector's own count.
- **The packet's stale line** said `spec_query` parsed 11 invariants; there are
  13. Packet artifact, not a doc defect — re-run to confirm.
- **INV-2's parity oracle** must implement § 2.4's id-in-leading-slot
  refinement, which the spec calls "this spec's, not § 7.2's". State that the
  survey already does, or that § 7's fold-in must make it.

## Resume artifacts

| Artifact | Path |
|---|---|
| Shared context packet | `<scratchpad>/cold-eyes-3757/shared-context.md` |
| Scrubbed doc copy (rebuild from current bytes before reuse) | same dir |
| Loop-1 fix ledger | `<scratchpad>/apply-fixes-ledger.json` |

Scratchpad root:
`/tmp/claude-1000/-mnt-Games-Scripts-Linux-Ants-Terminal/13f97878-fce8-4b3a-9d30-88f9952b510d/scratchpad`

## Delete this file when the run converges.
