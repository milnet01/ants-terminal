# Feature: Status-bar elision policy

## Contract

The status bar uses `ElidedLabel` to cap the pixel width of labels that
accept unbounded user-supplied strings (git branch name, foreground
process, transient notification). Elision with "…" is a *last resort* —
the label must show the full text whenever the layout gives it enough
room.

### Invariants

1. **Short text never elides.** Given an `ElidedLabel` with
   `maximumWidth = W` and `fullText = T` where `fontMetrics.width(T) ≤ W`,
   the displayed text MUST equal `T` regardless of the layout's
   squeeze behavior. A status-bar label reading "main" as "…" is a
   regression — the user sees no information where full content fit.

2. **Over-cap text elides to the cap.** Given `fontMetrics.width(T) > W`,
   the displayed text MUST be `T` truncated to fit W pixels, with the
   cap enforced by the elision mode (`ElideRight`, `ElideMiddle`,
   `ElideLeft`). Tooltip MUST carry the full string so hover reveals
   the un-elided form.

3. **Minimum sizeHint respects the text.** `minimumSizeHint()` MUST
   return at least the full-text width when the full-text width is ≤
   `maximumWidth()`. This prevents a parent layout (QStatusBar's
   QBoxLayout, in particular) from squeezing the widget below the
   width required to show the text in full.

4. **Minimum sizeHint respects the cap.** When the full-text width
   exceeds `maximumWidth()`, `minimumSizeHint()` MUST return
   `maximumWidth()` (not larger) — so a 10-char cap on a 200-char
   branch name doesn't make the widget demand 2000 px of statusbar.

5. **Tooltip reflects elision state.** When the displayed text
   differs from the full text (elision happened), the tooltip MUST
   be the full text. When displayed matches full (no elision), the
   tooltip MUST be empty — no stale tooltip from a prior over-cap
   string.

## Rationale

User report (2026-04-16): git branch chip displayed "…" even when the
branch name was "main" and the statusbar had plenty of empty space.
Root cause: `ElidedLabel::minimumSizeHint()` returned 3 characters'
width so the QStatusBar compressed the chip down to that minimum
regardless of the actual text length, then `fontMetrics().elidedText`
fit the compressed width with just "…". The fix promotes the minimum
to the full-text width (capped at `maximumWidth`), so short text is
guaranteed to render in full.

## Scope

### In scope
- `ElidedLabel::setFullText` + `minimumSizeHint` + `sizeHint` +
  `QLabel::text()` behavior under all four combinations of short/long
  text × capped/uncapped width.
- Elision policy per mode (right/left/middle): the displayed-text
  byte length matches the policy when cropped.
- Tooltip state.

### Out of scope
- QStatusBar's specific layout algorithm — we rely on Qt's
  documented behavior (QBoxLayout respects minimumSizeHint as a
  floor).
- Rendering / painting — we check `text()` and sizes, not pixels.
- Font fallback / Nerd-Font rendering — a separate concern.

## Regression history

- **0.6.28**: ElidedLabel introduced with minimumSizeHint =
  3 chars × averageCharWidth. Intended to let long strings elide
  inside stretch layouts, but made short text vulnerable to
  layout-squeeze to "…".
- **0.6.29**: minimumSizeHint promoted to full-text width (capped
  at maximumWidth). This spec + test lock the new policy.
