# Feature: a wrapped amend_body match is re-wrapped

## Invariants

**INV-1 — the joined line is re-wrapped.** On a store-backed project, an
`amend_body` whose `old_text` spans a line break (ANTS-4550) joins the
spanned lines. The joined line is then re-wrapped at word boundaries to
the width of the body's longest other line, keeping its own indentation.
The reply carries `rewrapped:true`.

**INV-2 — nothing else is re-wrapped.** A match inside one line, or a body
with fewer than two other lines or no width between 40 and 120 columns,
is left as written and carries no `rewrapped`.

## Rationale

ANTS-4970. The render does not re-wrap, so a two-word correction across a
line break left one line several times the width of its neighbours, and
a hand fix is discarded by the next write. The markdown path is not
covered: there the bullet's own line widths are not isolated, and nearly
every project is store-backed.

## Test surface

`test_roadmap_amend_body_rewrap.cpp` migrates a fixture into a sandboxed
store and drives `RemoteControl::cmdRoadmapLogAmendBodyForTest`.

## Regression history

- **ANTS-4970:** a wrapped match left one over-long line.
