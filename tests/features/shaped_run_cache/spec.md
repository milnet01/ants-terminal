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
- `size()`, `capacity()`, `hits()`, `misses()` — observability.

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

## Tests

`test_shaped_run_cache.cpp` (in the `test_vt` GUI bundle — shaping needs the
offscreen font database). One `TEST(ShapedRunCache, ...)` per invariant.
