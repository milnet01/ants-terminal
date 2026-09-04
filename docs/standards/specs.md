<!-- ants-specs-standards: 1 -->
# Spec-authoring standard

**Status:** v1 (2026-05-21).
**Applies to:** every spec under `docs/specs/` — the
`<ID>-<topic>.md` shape § 2 requires, and the bare `<ID>.md` of
legacy files.

**Why this is a full standard and not a `spec-format-overrides.md`.**
`~/.claude/standards/spec-format.md` says it is the only copy and that a
project needing to differ writes deltas only. That rule targets projects
carrying a *verbatim copy* that silently forked; this file is not one.
The two prescribe different documents — the global standard requires
twelve sections (Goal, Scope decisions, Design, Failure modes,
Alternatives considered, What checks this, …) where this one requires
six opening sections plus two more when their triggers apply (§§ 3-4),
and names the design section *Surface* — so the "deltas" would be
almost the whole file. It is also **executed, not just read**:
`src/speclint.cpp`, `src/specparse.h` and `src/speclog.h` implement the
rules below, and the `spec_lint`, `mcp_spec_query` and
`spec_parse_test_surface` feature tests lock them. Read
the global standard for anything this file does not cover; where they
differ on the *shape* of a spec under `docs/specs/`, this file wins.
**The one exception is § 1** — whether a spec is needed at all is
`spec-format.md` § 1's call, not this file's.

A *spec* is the implementation contract for one ROADMAP item. It sits
between the one-paragraph ROADMAP bullet (the *what* and *why*) and the
code (the *how*, in detail). It is the document a reviewer reads to
answer "is this design right?" before any code exists, and the document
a future maintainer reads to answer "why is the code shaped this way?".

This standard codifies the format the corpus already follows so authors
stop reverse-engineering it from old specs. It also keeps specs
parseable by the `spec_query` MCP tool (§ 6).

---

## 1. When a spec is required

Write a spec when the work has a **non-obvious contract** — invariants
a future change could silently break, a data shape other code depends
on, a security boundary, or a multi-file design.

Skip the formal spec (a regression test is more useful) when the work
is mechanical: a typo, a one-line fix, a menu entry, a dependency bump.
A `spec.md` under `tests/features/<name>/` (the feature-conformance
contract) is the right home for a small, single-invariant behaviour;
a top-level spec under `docs/specs/` is for designs big enough that
the contract spans files.

**If you are two files in and it is still spreading, stop and write the
spec.** That is the correction for a call that went the wrong way, and
it is cheaper than finishing it wrong. There is deliberately no "when
unsure, write the spec" rule — that instruction biased every borderline
call toward a document plus its full review gate. `spec-format.md` § 1
owns the five triggers and the skip test; read it rather than deciding
from this paragraph.

## 2. File + naming

- One spec per file at `docs/specs/<ID>-<topic>.md`, where `<ID>` matches
  `^ANTS-[0-9]+$` (the stable ROADMAP id from `.roadmap-counter`) and
  `<topic>` is two to four kebab-case words.
- **The topic slug is required on new specs** (user decision, 2026-07-30).
  A directory of bare ids is navigable only by someone holding the id;
  people remember names. `spec_query` resolves `<id>-*.md` as well as
  `<id>.md` (ANTS-3356), so both shapes are addressable by id and the
  slug costs nothing mechanically.
- Bare `<ID>.md` remains valid for the specs already written in that
  shape. Renaming them is a corpus-wide sweep with its own dry-run
  script, tracked by **ANTS-3755**; do not rename them piecemeal, which
  would leave the corpus half-converted with no record of which half.
- The spec elaborates exactly one ROADMAP bullet. Cross-cutting work
  that spans several ids gets one spec per id, cross-referenced in the
  header, or one umbrella spec whose header lists the ids it covers.

## 3. Required structure

Every spec opens with these, in this order. They are not the whole
required set — § 4 carries two more: **§ RAM / build cost** when its
trigger applies, and **§ Cold-eyes loop log** always.

The block below is that rule in the form `spec_lint`'s `missing_section`
check reads (ANTS-4345). It **adds no obligation** — every entry is
already required by this section or by § 4, and it is deliberately a
*subset* of them:

- **§ 3.1 Title and § 3.2 Header block are absent** because they are an
  `#` heading and bold key-value lines, not `##` sections, so the check
  cannot address them. `spec_query` already fails loudly on both.
- **§ RAM / build cost is absent** because § 4 requires it *conditionally*
  ("whenever its trigger applies"), and a flat list cannot carry a
  condition. Listing it would convert a conditional rule into an
  unconditional one, which is a change this standard has not made.
- **The entries carry no section numbers, on purpose.** This standard
  does not fix them — § 4's optional sections are inserted wherever they
  read best, which shifts every heading after them — so the check matches
  these by name at whatever number a spec gives them. A standard whose
  numbering *is* part of a section's identity writes the numbers in and
  gets an exact match instead; the global `spec-format.md` does exactly
  that.

<!-- required-sections -->
```
## Problem
## Surface
## Invariants
## Tests
## Cold-eyes loop log

Moved to [`docs/reviews/specs-review-log.md`](../reviews/specs-review-log.md).
A standard carries rules; its review history is read far less often and
was the larger half of this file.
