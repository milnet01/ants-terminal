# Feature: the OSC window title is bounded

## Problem

`TerminalGrid::handleOsc` accepts OSC 0 and OSC 2 — set window title —
and assigns the sanitised payload straight to `m_windowTitle` with no
length bound. The payload rides `VtParser`'s OSC accumulator, which is
sized in megabytes, so a single escape sequence in a file the user
`cat`s sets a title of that size.

That title is not confined to the grid. `TerminalWidget` emits it as
`titleChanged`, which reaches the window manager and the tab bar, and
`SessionManager` writes it into the session blob and reads it back into
`TerminalGrid::setTitle` on the next launch. So the effect outlives the
process that caused it: the oversized title is restored at every start
until the session is discarded.

Every other attacker-supplied OSC payload in this file is bounded — the
OSC 8 hyperlink URI and its id, the OSC 52 clipboard write, the OSC
1337 user variable, the OSC 777 notification title and body, and the
inline-image base64. The window title was the outlier.

## Contract

A window title is a short human-readable label. It MUST be bounded on
assignment, and the bound MUST cover both ways in:

1. `handleOsc`'s OSC 0 / OSC 2 ingress.
2. `setTitle`, which is what `SessionManager` calls when restoring —
   otherwise a session blob written by an earlier build reintroduces
   an unbounded title that no OSC could set any more.

Making `setTitle` the one choke point satisfies both, and is what this
feature requires rather than two separate caps that can drift.

The bound is 1024 UTF-16 code units. Far past any real title — the
sibling OSC 777 notification title in the same function caps at 256 —
and far below a size that costs anything at the window manager, the tab
bar, or in the session file.

Truncation MUST NOT leave a lone surrogate at the end. Cutting at a
code-unit boundary can split a surrogate pair, and half a pair is not a
character; it would be handed to the window manager as an unpaired
code unit.

## Invariants

**INV-1 — an over-long OSC 2 title is truncated.** After an OSC 2
payload whose title part exceeds the bound, `windowTitle()` is no
longer than the bound.

**INV-2 — an ordinary title is untouched.** A short title round-trips
through OSC 2 exactly, so the cap does not disturb normal use.

**INV-3 — OSC 0 is bounded too.** Both title-setting OSC numbers go
through the same path.

**INV-4 — the restore path is bounded.** `setTitle` called directly
with an over-long string — the shape `SessionManager` produces when
reading a blob written before this fix — yields a title no longer than
the bound.

**INV-5 — truncation leaves no lone surrogate.** A title built from
astral-plane characters, cut so that the bound falls inside a surrogate
pair, ends on a complete character.

## Scope

### In scope
- Runtime test driving `TerminalGrid::processAction` with an
  `OscEnd` payload, as `osc_color_query` does, plus a direct
  `setTitle` call for the restore path.

### Out of scope
- Truncating titles already inside session blobs on disk. They are
  bounded when read, by INV-4, which is the same guarantee without
  rewriting the user's saved sessions.
- Rate-limiting title changes. A title set repeatedly is a different
  concern from one set enormous; OSC 52 and OSC 1337 carry their own
  rate limits and the title does not.
- The tab bar's own rendering cost for a long title. Bounded input
  makes it moot.

## Regression history

- **ANTS-1667:** stripped C0, DEL and C1 controls from the title. Left
  the length unbounded.
- **ANTS-4456 (cold sweep 2026-08-18, verified 2026-09-08):** reported
  as "OSC 0/2 window title is the one attacker-supplied OSC payload
  with no length cap and is persisted to the session file". Verified
  against source and fixed. Locked by this spec.
