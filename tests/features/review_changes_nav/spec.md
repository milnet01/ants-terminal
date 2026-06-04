# Review Changes dialog: in-dialog navigation (clickable files + back-to-top)

## Problem

The Review Changes dialog (`diffviewer::show`, `src/diffviewer.cpp`)
renders Status, Branches, Diff and New-files sections into a single
read-only text view. On a busy repo the Diff section is long, so:

1. The user sees a file in the **Status** list but has to hand-scroll
   to find that file's hunk further down in the **Diff** section.
2. The **Diff** section opens with a `git diff --stat` summary listing
   every changed file, but those names are inert — you still scroll to
   reach the patch.
3. Once scrolled deep into a long diff there is no quick way back to
   the top.

User request 2026-06-03 (ANTS-1965 / ANTS-1966 / ANTS-1967).

## Fix

### Clickable filenames (ANTS-1965 + ANTS-1966)

The viewer is a `QTextBrowser` (a `QTextEdit` subclass — the existing
scroll-preservation logic is unaffected) with internal-anchor
navigation:

- `setOpenLinks(false)` + `setOpenExternalLinks(false)` so the browser
  never tries to launch an external app or open a file on click.
- `anchorClicked(const QUrl&)` is connected to a handler that, for a
  fragment-only URL (`#…`), calls `scrollToAnchor(fragment)` — jumping
  within the same document.

Each changed file gets a deterministic, anchor-safe id from
`fileAnchorId(path)` (hex-encoded UTF-8 of the repo-relative path —
injective, always `[0-9a-f-]`). For every file:

- A **target** anchor `<a name="f-…"></a>` is emitted at the file's
  patch in the Diff section (immediately before its
  `diff --git a/… b/…` line) and at each New-file synthetic patch.
- A **link** `<a href="#f-…">path</a>` wraps the file path in the
  Status list (ANTS-1965) and in the Diff `--stat` file list
  (ANTS-1966).

A link is only emitted for a path that actually has a target anchor
(the set is computed up front from the diff's `b/<path>` lines plus the
untracked New-files list), so there are no dead links. Paths that don't
appear in the diff (e.g. a rename's old name, or a quoted path with
embedded spaces) render as plain text — graceful no-op, never a broken
link.

### Back-to-top button (ANTS-1967)

An overlay `QPushButton` (`reviewBackToTopBtn`) parented to the viewer,
mirroring the terminal's scroll-to-bottom chip:

- Hidden on open; shown (top-right of the viewer) once the vertical
  scroll bar moves off 0; hidden again at the top.
- Repositioned on viewer resize and on scroll so it stays pinned to the
  top-right corner.
- Click scrolls the viewer's vertical scroll bar back to 0.

## Invariants

Source-grep harness over `diffviewer::show` — no display required.

- **INV-1** (browser viewer): the viewer is constructed as a
  `QTextBrowser`, not a bare `QTextEdit`. The anchor-navigation and
  `anchorClicked` signal require `QTextBrowser`.
- **INV-2** (links don't escape): `setOpenLinks(false)` is set on the
  viewer so an internal `#fragment` click cannot be routed to an
  external handler.
- **INV-3** (click → scroll): `anchorClicked` is connected and the
  handler calls `scrollToAnchor(...)` — the actual jump.
- **INV-4** (anchor id helper): a `fileAnchorId(` helper exists and is
  used to build both the `name=` targets and the `href="#` links, so
  link and target ids are derived identically.
- **INV-5** (target anchors): the render emits `<a name=` anchors keyed
  by `fileAnchorId` in the diff/new-files rendering.
- **INV-6** (link emission): the render emits `href='#"` /
  `href=\"#"`-style fragment links built from `fileAnchorId` for the
  Status and diffstat file lists.
- **INV-7** (back-to-top exists): a back-to-top `QPushButton` is created
  with object name `reviewBackToTopBtn`.
- **INV-8** (scroll-driven visibility): the back-to-top button's
  visibility is wired to the viewer's vertical scroll bar
  (`verticalScrollBar()` + a `valueChanged` connection) — shown when
  scrolled, hidden at the top.
- **INV-9** (back-to-top action): clicking the button drives the
  vertical scroll bar to 0 (`setValue(0)` or `scrollToAnchor` to the
  document top).

## How to verify pre-fix code fails

```bash
git stash   # or check out the pre-ANTS-1965 src/diffviewer.cpp
cmake --build build --target <bundle-with-this-test>
ctest --test-dir build -R review_changes_nav
# Expect: INV-1..9 fail — pre-fix source uses a bare QTextEdit with no
# anchors, no anchorClicked wiring, and no back-to-top button.
```
