# Architecture Decision Records (ADRs)

This folder holds architecture decision records — short markdown
files capturing **why** a significant architectural choice was
made, what alternatives were considered, and what the trade-offs
were.

The format is Michael Nygard's lightweight pattern (see
[ADR-0001](0001-record-architecture-decisions.md)): one file per
decision, numbered sequentially, never edited after acceptance
(superseded decisions get a new ADR that references the prior
one).

## When to write an ADR

Write one when a decision:

- Has long-term consequences (months or years).
- Closes off alternatives that future contributors might propose.
- Reflects a trade-off that isn't obvious from the code alone.
- Required real research / debate to settle.

Don't write one for:

- Small refactors that anyone might revisit on a Tuesday.
- Choices forced by external constraints (compiler version, OS
  API surface) — the constraint *is* the rationale; mention it
  in code comments instead.
- Decisions captured fully in `CLAUDE.md` or one of the
  standards docs — those are the right home for repeatable
  rules.

## Numbering

Sequential, zero-padded to 4 digits:
`0001-record-architecture-decisions.md`,
`0002-<topic>.md`, …

Append-only — once an ADR has a number, it keeps it forever,
even if superseded.

## Co-decider convention

ADRs in this project list both "Project lead" and "Claude" as
**Deciders** when the architectural choice emerged from a
collaborative design session (the project lead steered the
requirements; Claude Code provided option analysis, trade-off
enumeration, and implementation feasibility checks). Listing Claude
reflects that the decision is not purely the project lead's alone —
the LLM's analysis shaped the outcome. It does **not** imply Claude
can make decisions unilaterally; the project lead always has final
sign-off.

## Lifecycle

| Status | Meaning |
|--------|---------|
| Proposed | Drafted; under discussion. |
| Accepted | Decision made; in effect. |
| Deprecated | No longer applies; new code shouldn't follow it. |
| Superseded by ADR-NNNN | Replaced wholesale by a later decision. |
| Amended by ADR-NNNN | Body still in effect except as noted by the cited ADR (partial supersession). |

A status change is an edit to the ADR's `Status:` field; the
body of an accepted ADR isn't rewritten — supersession or
amendment is captured in a new ADR.

**Exception — outcome annotations.** A brief `**Outcome
(YYYY-MM-DD):**` block appended to a Decision bullet is
sanctioned. It documents what *actually shipped* vs what was
projected; this is read-only history, not a policy change.
Keep it one paragraph; if the actual outcome differs
substantially from the projection, write a superseding ADR
instead.

The combined form `Accepted (Amended by ADR-NNNN)` is also used
in practice when an ADR has been accepted and later partially
amended — the `Accepted` prefix signals the original decision
is still in effect; the parenthetical names which ADR amended it.

## Template

See [ADR-0001](0001-record-architecture-decisions.md) — both the
canonical first ADR and a worked example of the format.
