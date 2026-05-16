# roadmap_parser_blank_line_continuation — ANTS-1426

## Problem

`RoadmapDialog::parseBullets` terminates the bullet-body
collection loop at the first blank line
(`roadmapdialog.cpp:562`). That's stricter than CommonMark's
loose-list mode, which allows a blank line *within* a list item
when the next non-blank line is still an indented continuation.

The ANTS-1422 entry in this project's `ROADMAP.md` shipped with a
blank line between its "pull 7" and "pull 1" sub-blocks (for
human readability). The parser stops at the blank line, so the
`Kind: fix.` line at the bottom of the body never reaches the
regex match — `roadmap_query` returns `kind:""` for that bullet.

## Goal

Relax `parseBullets`'s body-collection loop: when encountering a
blank line, peek ahead to the next non-blank line. If that line
is an indented continuation (`  …` and not a new top-level
`- ` / `* ` bullet), treat the blank line as part of the body
and keep collecting. Any other follow-up line still terminates.

## Invariants

- **INV-1.** Bullet body with no blank lines (current common
  case) parses identically — `Kind` / `Lanes` / `Layman`
  extracted from the existing format.
- **INV-2.** Bullet body with a blank line followed by an
  indented `Kind:` continuation parses correctly — `kind`
  field set.
- **INV-3.** A blank line followed by a *new* top-level bullet
  (`- ` or `* ` at column 0) still terminates the body — the
  next bullet must be its own entry.
- **INV-4.** A blank line followed by a heading (`#`, `##`,
  `###`) still terminates the body.
- **INV-5.** EOF after a blank line terminates the body
  cleanly (no crash, returns the bullet up to that point).

## Test scope

Behavioral test against `RoadmapDialog::parseBullets` with five
synthetic fixtures, one per invariant.
