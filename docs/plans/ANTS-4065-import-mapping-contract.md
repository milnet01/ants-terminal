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

**A2. Wipe the store.**
`rm ~/.local/share/ants-terminal/roadmap.sqlite`

Safe and reversible: it is derived, gitignored and machine-local, and Phase D
rebuilds it. Doing it now rather than later stops a stale row surviving into a
verification run.

> **Verify:** `roadmap_query` still answers (markdown path), and
> `roadmap_log op:"annotate"` succeeds without a `render_gate_unmet` refusal —
> proving the project is back on the markdown backend.

---

## Phase B — Audit the source, now that it is the source

Order matters within the phase: B1 makes B2's numbers trustworthy.

**B1. Re-run the corpus survey against pre-render inputs.**
Every figure in circulation — including `roadmap-data-model.md` § 7.4's "11
others" — came from a survey run *after* the render, which is why `bug` (29
items) is invisible in all of them.

> **Verify:** the survey's `Kind:` inventory now lists `bug`, and its
> non-canonical count is ≥ the spec's seven additions.

**B2. Fix the format defects the audit already found.**
- 44 open-item headlines with no terminating period (`roadmap-format.md` § 3.5
  requires one).
- 35 headlines containing newlines; 5 over 200 characters.
- The 99 bullets writing `Kind:` inline stay as they are — § 2.2 makes that
  shape supported rather than accidental.

> **Verify:** re-run the field auditor; `headline_no_period` and
> `headline_multiline` both report 0 for open items.

**B3. Settle the three open rulings.** They are decisions, not code:
`feature/fix`, `design + implement`, `design + fix` (spec § 2.1), and the
`priority` severity scale (§ 5). Until ruled, each has stated interim behaviour,
so this step can land after Phase C without blocking it.

> **Verify:** each ruling is written into `roadmap-data-model.md` § 7.4, not
> into the spec — § 7.4 is the mapping's only home.

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

**D1. Dry-run the import against the reverted roadmap.**

> **Verify:** `items_orphaned` 0. Every `Kind: bug` bullet reports `kind='fix'`
> with `extras.source_kind='bug'`. No `field_defaulted` note names a field the
> bullet visibly declares — spot-check against the 48 inline-`Kind:` items.

**D2. Run it for real, then diff store against source.**

> **Verify:** no item's `kind` differs from the value its bullet declares. This
> is the check that would have caught the original 123 rewrites, and it is the
> one that says Phase C worked.

**D3. Render, then re-import — the acceptance test (INV-6).**

> **Verify:** `items_updated == 0` over the nine governed columns and no
> `field_conflict` naming one. **`headline`, `layman` and `lanes` are expected to
> fail here** — they are the spec's named undiagnosed drift, and this run is the
> instrument for diagnosing them. Anything *else* failing is a Phase C defect.

**D4. Diagnose the residual drift** those three columns show, and either fold
the cause into the spec as a new § 2.x (re-gating the amendment per rule 14) or
split it into its own item.

> **Verify:** INV-6 green across all nine columns, on two consecutive
> render→import cycles rather than one — a single pass cannot distinguish
> "stable" from "drifting slowly".

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
- **It does not back-fill `Kind:`** onto the 1,613 corpus items that carry none.
  They default legitimately; Phase C's note makes the default visible, which is
  all this work owes them.
