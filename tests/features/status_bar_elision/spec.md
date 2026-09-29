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

3. **Minimum sizeHint respects the text.** For a label with a
   `maximumWidth()` set, `minimumSizeHint()` MUST return at least the
   full-text width when the full-text width is ≤ that cap. An uncapped
   label takes only a small floor (invariant 6). This prevents a parent layout (QStatusBar's
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

6. **An uncapped message slot never raises the window's minimum
   width.** With the status-message slot built as `MainWindow` builds
   it (an `ElidedLabel`, `Qt::ElideMiddle`, stretch 1, no
   `maximumWidth`), setting a message ~3000 px wide MUST NOT raise the
   window's or the status bar's `minimumSizeHint().width()` with the
   message: it stays well under the message's width and is the same for
   a message twice as long. The window keeps the width it was given,
   the slot's displayed text is elided (contains "…", keeps the start
   and end of the message, tooltip carries the full text), and a capped
   short-text chip beside it is still shown in full. Asserting the
   elision as well as the width means a fix that merely hides the text
   cannot pass. The test replays the setters found in `MainWindow`'s
   real construction of the slot and fails on one it cannot replay, so
   the test cannot drift from the real slot.

## Rationale

User report (2026-09-29): the main window widened itself into a very
wide, short shape. Cause: the uncapped status-message `ElidedLabel`
returned the full text's width as `minimumSizeHint`, so a long message
raised the `QStatusBar`'s and the window's minimum width (INV-6).

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
- **2026-09-29**: the uncapped status-message slot was found to widen
  the window (INV-6 added).
