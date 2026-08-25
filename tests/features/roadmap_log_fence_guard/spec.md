# roadmap_log fence guard (ANTS-3640)

Status: shipped
Kind: fix
Source: in-session-2026-07-26 (hit live while triaging cross-session feedback)

## Problem

`roadmap_log` writes a bullet's `body` (and an `op:annotate` / `op:flip`
`note`) as continuation lines indented two spaces under the bullet. That
indent stops nothing: CommonMark opens a fenced code block at
`^ {0,3}(```|~~~)`, and `remotecontrol.cpp`'s own `walkGfmBullets` /
`walkAntsV1Bullets` toggle their fence state on any `trimmed()` line
starting with ```` ``` ```` at any indent.

So a body that quotes a fence opener without closing it silently fences off
every bullet below it. It happened for real: ANTS-3635's body quoted one
opener, ANTS-3638's quoted another, and the pair swallowed ANTS-3637
between them. The damage surfaced two operations later as

    anchor_unsafe_context: located bullet is inside a fenced code block

pointing at the innocent bullet being edited rather than at the append that
broke the file — so the reader looks in the wrong place. Repairing it took
a `grep -n` and a hand edit.

## Surface

- `rcEscapeUnclosedFence(QString &)` — new, called from the tail of the
  shared `rcScrubLeakedToolXml`, so every writer that already scrubs prose
  (`op:append`, `append_batch`, `flip` note, `annotate`, `amend_body`, the
  pass-format renderer, `changelog_log`) is covered by one chokepoint.
- `rcFenceOpenerHint(int)` — new, appended to the six
  `anchor_unsafe_context` refusal messages.
- `GfmBullet::fenceOpenLine` / `AntsV1Bullet::fenceOpenLine` — new fields,
  tracked alongside the existing `insideFenced`.

## Invariants

- **INV-1** — an `op:append` body whose line opens a fence it never closes
  is written with a CommonMark backslash escape (`\```), and a bullet
  appended below it stays editable (`op:flip` succeeds).
- **INV-2** — a body containing a *balanced* fence pair is written
  verbatim. Quoting a whole code block leaves the file well-formed and is
  a legitimate thing to write, so the guard does not touch it.
- **INV-3** — the same escape applies to an `op:annotate` note, because
  both paths run through `rcScrubLeakedToolXml`.
- **INV-5** (ANTS-4572) — a scrub that removed ANYTHING says so. The
  `body_scrubbed_tool_xml` warning fired only on a matched
  `<parameter name="X">` pair, so a stray closing tag or ANTS-4609's
  `<tag>scalar` line was stripped in silence; the envelope now carries
  `unnamed_fragments_removed` alongside (or instead of) `lost_parameters`.
  Cosmetic normalisation — blank-run collapse, trailing whitespace, the final
  newline chop — is deliberately NOT counted: it fires on almost every body and
  would turn the signal into noise. Belongs here because the scrub and the
  fence escape are one helper, which is also why INV-3 lives here.
  *Test:* `roadmap_log_fence_guard.Ants4572UnnamedScrubIsReported`.
- **INV-4** — when a bullet genuinely sits inside a fence (a hand-written
  file the guard never saw), the `anchor_unsafe_context` refusal names the
  1-based line number of the fence *opener*, not just the bullet.

## Tests

`tests/features/roadmap_log_fence_guard/test_roadmap_log_fence_guard.cpp`,
one `TEST` per invariant, driving the `*ForTest` handlers against a seeded
`QTemporaryDir` roadmap (same harness as `roadmap_log_trailing_whitespace`).

Must-fail-first: with `rcEscapeUnclosedFence` stubbed out, INV-1 fails at
the `op:flip` (refused `anchor_unsafe_context`) and INV-3 fails on the
written text; with `rcFenceOpenerHint` returning `{}`, INV-4 fails.
