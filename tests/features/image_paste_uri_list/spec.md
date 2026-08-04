# Image paste accepts `text/uri-list`, not just a raster

**ID:** ANTS-3828
**Status:** shipped
**Kind:** fix
**Source:** user-report-2026-08-04

## Problem

`Ctrl+Shift+V` in `TerminalWidget::keyPressEvent` branched on
`mime->hasImage()` only. Two clipboard payloads therefore took two
different paths:

- **Screenshot to clipboard** (Spectacle's default) puts an `image/png`
  raster on the clipboard. `hasImage()` is true, the image is saved
  under the validated paste directory, and the bare filepath is pasted.
  This was correct and is unchanged.
- **Copying an image *file*** (Dolphin, "copy") puts `text/uri-list` +
  `text/plain` on the clipboard and **no raster**. `hasImage()` was
  false, so the paste fell through to the ordinary text branch and wrote
  `file:///home/…/shot.png` verbatim to the PTY.

Claude Code cannot resolve a `file://` URI as a path, so the attachment
never formed — what the user saw as a dead `[Image 1]` placeholder.

## Surface

`TerminalWidget::imagePathsFromUrls(const QList<QUrl> &)` — a public
static helper that maps a `text/uri-list` payload to the text to paste,
or an empty string when the payload carries no local image file.
`keyPressEvent` calls it from a `mime->hasUrls()` branch placed **after**
the `hasImage()` branch and **before** the plain-text fallback.

## Invariants

- **INV-1** — a local image URL yields its bare filesystem path, not a
  `file://` URI. No file is written: the file already exists on disk, so
  the save step and the paste-directory canonicalisation that guards it
  do not apply.
  *Test:* `BareLocalImagePath`.
- **INV-2** — a path outside the shell-safe character set is passed
  through `shellQuote()`. The filename here is whatever the file is
  called on disk, and `pasteRiskReasons()` does not flag `;` or `$(…)`,
  so an adversarially-named download would otherwise reach the shell
  unquoted. Ordinary paths are **not** quoted, so the common case still
  pastes the bare path Claude Code expects.
  *Test:* `QuotesUnsafePath`, `LeavesOrdinaryPathBare`.
- **INV-3** — a non-image local file, a remote URL, and an empty URL
  list all yield an empty string, so the caller falls through to the
  existing text paste. Widening the fix to non-image files is a separate
  decision, deliberately not taken here.
  *Test:* `IgnoresNonImageFile`, `IgnoresRemoteUrl`, `IgnoresEmptyList`.
- **INV-4** — multiple image URLs yield one space-separated line, each
  element independently quoted.
  *Test:* `JoinsMultipleImages`.
- **INV-5** — the `hasUrls()` branch is wired into the `Ctrl+Shift+V`
  handler and sits after the `hasImage()` branch. Deleting either the
  call or the ordering silently restores the bug.
  *Test:* `HandlerWiredAfterRasterBranch`.

## Scope

The raster (`hasImage()`) path is not exercised here. Driving it needs a
live `QClipboard` with an `image/png` payload and a constructed
`TerminalWidget` — a QOpenGLWidget with a live PTY — and this fix does
not change that path. INV-5's ordering check is what pins the two
branches' relationship.

## Red-first proof

`HandlerWiredAfterRasterBranch` scrapes `src/terminalwidget.cpp` at
runtime, so it can be, and was, run against the pre-fix source
(`git show HEAD~:src/terminalwidget.cpp`) and fails there. The
behavioural tests are red against pre-fix code by construction — the
helper they call did not exist.
