# Feature: changing a per-style font family invalidates the shaped-run cache

## Problem

`TerminalWidget` caches shaped `QTextLayout`s in `m_shapedRunCache`
(ANTS-3453), so a run whose text is unchanged frame-to-frame draws
without a HarfBuzz re-shape. The cache key is the run's text and a
two-bit style variant — bold, italic — and nothing else. The font is
passed to `layoutFor` only on a miss, to do the shaping.

That key is correct as long as the font behind each variant does not
change. Three setters change it:
`setBoldFontFamily`, `setItalicFontFamily` and
`setBoldItalicFontFamily`, all reachable from Settings. Each rebuilds
its `QFont` and calls `update()`, and none clears the cache.

So after picking a new bold font, every bold run already in the cache
keeps drawing in the old family. The paint is not stale in the usual
sense — it repaints immediately — it repaints from a layout shaped with
the previous font. The setting looks ignored until something else
clears the cache, which today means `updateFontMetrics`: a font-size
change, or a resize.

`updateFontMetrics` does clear it, which is why the base font family
setter has never shown this.

## Contract

Any setter that replaces the `QFont` behind a cached style variant MUST
clear `m_shapedRunCache` before requesting a repaint. The cache key
cannot distinguish the fonts, so invalidation is the only correctness
mechanism available to it.

Widening the key to include the font is the alternative and is not
taken here: it would add a comparison to every cache lookup on the hot
paint path to serve an event that happens when a user opens Settings.

## Invariants

**INV-1 — `setBoldFontFamily` clears the shaped-run cache.**
**INV-2 — `setItalicFontFamily` clears the shaped-run cache.**
**INV-3 — `setBoldItalicFontFamily` clears the shaped-run cache.**

Source-grep each function body in `src/terminalwidget.cpp` for a
`m_shapedRunCache.clear()` call.

**INV-4 — `updateFontMetrics` still clears it.** The three above are
additions to an existing guarantee, not a replacement for it; this
invariant stops a later edit from moving the clear rather than adding
to it.

## Scope

### In scope
- Source-grep over `src/terminalwidget.cpp`.

### Out of scope
- A rendering test that changes the bold family and compares pixels.
  It needs a real paint device and a font whose difference is
  measurable on this host; the E2E harness with `grab-image` is where
  that would go, not the unit bundle.
- Widening the cache key. Rejected above, with the reason.
- Any other cache. This claim is about the shaped-run cache only.

## Regression history

- **ANTS-3453:** added the shaped-run cache, keyed by text and style
  variant, and cleared it in `updateFontMetrics`. The three per-style
  family setters were not considered.
- **ANTS-4456 (cold sweep 2026-08-18, verified 2026-09-08):** reported
  as "per-style font setters never clear the shaped-run cache".
  Verified against source and fixed. Locked by this spec.
