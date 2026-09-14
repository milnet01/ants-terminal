# ShapedRunCache — glyph/shaped-run cache (ANTS-3453)

## Problem

`TerminalWidget::paintEvent` re-shaped every text run through `QTextLayout`
on every frame, even when the run's text was unchanged frame-to-frame. The
HarfBuzz shape pass is the dominant per-frame cost and the root of the
multi-second typing freeze under heavy Claude Code output. `ShapedRunCache`
(`src/shapedruncache.{h,cpp}`) caches shaped layouts keyed by `(text,
variant)` so an unchanged run is drawn without re-shaping.

## Surface

- `QTextLayout *layoutFor(text, variant, font, fontAscent, baselineOffOut&)`
  — returns a laid-out layout, shaping on a miss and reusing on a hit;
  writes the cached `baselineOff` (the ANTS-2100 correction,
  `fontAscent - line.ascent()`).
- `clear()` — drops all entries (font/DPI/theme change).
- `size()`, `capacity()`, `hits()`, `misses()`, `cachedTextUnits()` —
  observability.

Eviction is generational (two maps, hot + cold): a lookup checks hot then
cold (promoting on a cold hit); when hot reaches capacity, cold is dropped
and hot becomes the new cold. Live entries are bounded to `2 x capacity`.

## Invariants

- **INV-1** — A repeated `(text, variant)` lookup is a hit: `misses()`
  counts distinct keys only; `hits()` counts the repeats.
- **INV-2** — A different `variant` (or different `text`) is a distinct
  entry — a miss, not a hit.
- **INV-3** — `baselineOffOut` for a key is stable: a hit returns the same
  value the miss computed, and the returned layout's text round-trips
  (`layout->text() == text`).
- **INV-4** — Generational eviction bounds `size()` to `<= 2 * capacity`
  across arbitrarily many distinct keys; every returned layout is non-null
  and valid; the most-recently-inserted key is a hit on immediate re-lookup.
- **INV-5** — `clear()` empties the cache (`size() == 0`) and the next
  lookup is a miss; the lifetime `hits()`/`misses()` counters are NOT reset
  by `clear()`.
- **INV-6** (ANTS-5077) — a run longer than `kMaxCachedRunUnits` UTF-16 units
  is shaped but not stored: `size()` does not grow, a repeat lookup is a miss
  again, and the returned layout is valid with its text round-tripping until
  the next `layoutFor` call.
- **INV-7** (ANTS-5077) — retained text is bounded by a budget, not only by
  the entry count: across many distinct runs each under the per-run cap,
  `cachedTextUnits()` never exceeds `2 * kTextUnitBudget`, while `size()` stays
  under `2 * capacity`. A space-free line is one whole-row run, so an entry cap
  alone let a few tabs of long runs hold tens of MB after output stopped.

The budget counts UTF-16 units (`QString::size()`), which is never less than
the codepoint count, so it bounds at least as tightly as a codepoint budget.

## Tests

`test_shaped_run_cache.cpp` (in the `test_vt` GUI bundle — shaping needs the
offscreen font database). One `TEST(ShapedRunCache, ...)` per invariant.
