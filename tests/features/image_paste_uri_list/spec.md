# Image paste accepts `text/uri-list`, not just a raster

**ID:** ANTS-3828
**Status:** shipped
**Kind:** fix
**Source:** user-report-2026-08-04; extended by ANTS-5580 (user-report-2026-09-30)

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

**ANTS-5580 extension.** The right-click menu's Paste and the middle-click
paste read the clipboard's plain text, which for a copied file is the
`file://` address, so the same bug returned by another route. The paste now
lives in one place: `TerminalWidget::pasteFromClipboard(QClipboard::Mode)`
(null guard, raster branch, then `pasteTextForMime` handed to
`pasteToTerminal`). `static TerminalWidget::pasteTextForMime(const QMimeData *)`
is the URL-versus-text decision and is what INV-8 and INV-9 drive. The
Ctrl+Shift+V branch of `keyPressEvent`, the Paste action in
`contextMenuEvent` and the middle-button branch of `mousePressEvent` (with
`QClipboard::Selection`) call `pasteFromClipboard` and nothing else. INV-5,
INV-6 and INV-7 were written when that code sat in `keyPressEvent`; they now
hold the same guarantees about `pasteFromClipboard`.

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
- **INV-5** — in `pasteFromClipboard`, the raster (`hasImage()`) branch
  comes before the `pasteTextForMime()` step, and that step's result is handed
  to `pasteToTerminal`. Deleting the call or the ordering silently restores
  the bug (a screenshot intercepted by the URL path, or the raw text pasted).
  Which of URL and text wins is INV-8 and INV-9.
  *Test:* `HandlerWiredAfterRasterBranch`.
- **INV-6** (ANTS-3831) — `pasteFromClipboard` guards
  `clipboard->mimeData(mode)` before dereferencing it, and the guard precedes
  the first `mime->` use. Qt documents the return as nullable ("can be
  `nullptr` if the given mode is not supported by the platform"), and the
  branches below it dereference it. The middle-click route passes
  `QClipboard::Selection`, so the unsupported-mode trigger is now reachable
  on platforms without a selection clipboard. Verified against the Qt 6 documentation before the guard
  was added, because the originating bullet recorded a suspicion rather than
  a reproduction; the item required that check and would have been closed
  `n/a` had the pointer been non-nullable.
  **No crash is demonstrated.** The invariant holds the documented
  contract, not an observed failure, and the cost is one branch.
  *Test:* `NullMimeDataGuardedBeforeAnyDereference`.
- **INV-7** (ANTS-5077) — in the raster (`hasImage()`) branch of
  `pasteFromClipboard`, a
  `QThread::create` worker runs `img.save(filename)` and narrows the saved
  file to owner-only with `setOwnerOnlyPerms(filename)`, so the PNG encode
  never runs on the GUI thread. The delivery back on the widget pastes
  `shellQuote(filename)` and emits `imagePasted` only after checking the
  worker's result. `imagePasted` is emitted nowhere else in
  `terminalwidget.cpp`, so a failed save announces nothing.
  *Test:* `RasterPasteIsPrivateQuotedAndAnnouncedOnSave`.
- **INV-8** (ANTS-5580) — a clipboard payload naming local image files
  (`text/uri-list`), even with a plain-text `file://` address beside it,
  pastes the bare local path(s) as `imagePathsFromUrls` returns them, with no
  `file://` in the result. Two image files paste joined.
  *Test:* `CopiedImageFilePastesBarePathNotFileUri`,
  `CopiedImageFilesPasteJoinedPaths`.
- **INV-9** (ANTS-5580) — every other non-raster payload pastes its plain
  text unchanged: text only; a remote URL plus its text; a local non-image
  file plus its text (INV-3's scope). A null payload, or one with neither
  urls nor text, pastes the empty string.
  *Test:* `PlainTextPastesUnchanged`, `RemoteUrlPayloadPastesItsText`,
  `LocalNonImageFilePayloadPastesItsText`, `NullOrEmptyPayloadPastesNothing`.
- **INV-10** (ANTS-5580) — the Ctrl+Shift+V branch of `keyPressEvent`, the
  Paste action in `contextMenuEvent` and the middle-button branch of
  `mousePressEvent` (with `QClipboard::Selection`) each call
  `pasteFromClipboard(`, and none reads the clipboard's text or mime data
  itself. Any route that does can paste a `file://` address again.
  *Test:* `AllThreeRoutesCallPasteFromClipboardAndNothingElse`.

## Scope

The raster (`hasImage()`) path is not driven here. Driving it needs a
live `QClipboard` with an `image/png` payload and a constructed
`TerminalWidget` — a QOpenGLWidget with a live PTY. INV-5's ordering check
pins the two branches' relationship, and INV-7 is a scrape of the raster
branch's save block. INV-10 is likewise a scrape: no widget or live
clipboard is constructed, so it proves each route calls the shared function,
not that the resulting bytes reach a PTY. INV-8 and INV-9 are behavioural but
call `pasteTextForMime` directly with a built `QMimeData`.

## Red-first proof

`HandlerWiredAfterRasterBranch` scrapes `src/terminalwidget.cpp` at
runtime, so it can be, and was, run against the pre-fix source
(`git show HEAD~:src/terminalwidget.cpp`) and fails there. The
behavioural tests of INV-1 to INV-4 were red against pre-fix code by
construction — the helper they call did not exist. ANTS-5580: INV-8 was
written red against a stub of `pasteTextForMime` returning the plain text;
the INV-5, INV-6, INV-7 and INV-10 scrapes fail until `pasteFromClipboard`
exists.
