# Feature: Status-bar Roadmap viewer

User request 2026-04-27: "Add a button on the status bar to view the
roadmap. So, it brings up a dialog showing the roadmap. It should
have filters as well to show what is outstanding and what is
completed if the user wants to use the filters. If at all possible,
it should also highlight what item is being done currently."
Follow-up clarification: "The roadmap button should only show if
there is roadmap documentation. Let's simplify that to requiring a
roadmap.md file only. The user should follow norms for the roadmap
button to show."

## Contract

A status-bar button labelled "Roadmap" appears whenever the **active
tab's working directory** contains a file matching `ROADMAP.md`
(case-insensitive). Clicking opens a dialog rendering the file with
filter controls and a current-work highlight. The button is hidden
otherwise — terminals open in directories without a roadmap pay
nothing.

## Trigger / visibility

- The button is owned by `MainWindow`. `refreshStatusBarForActiveTab`
  (the existing tab-switch hook) calls a new helper
  `refreshRoadmapButton()` which:
  1. Reads the active tab's terminal's current working directory
     (via the existing `TerminalWidget::currentWorkingDirectory()`
     accessor used by Review Changes).
  2. Probes the directory for a file whose basename, lowercased,
     equals `roadmap.md`. If found, retains the canonical path.
  3. Shows the button when found, hides otherwise.
- The button title is constant ("Roadmap") — no badge / count, since
  the roadmap is not a counter.
- Per-tab cwd: tab A may show the button while tab B doesn't, even
  in the same window. The label is fixed-width so no layout shift.

## Dialog

### Layout

- `QDialog` titled "Roadmap — <basename of canonical path>", non-modal
  (so the user can read while typing in the terminal).
- Top row: five `QCheckBox` filters in a `QHBoxLayout`. All
  default-checked. They are *peer categories* combined inclusively
  (a bullet renders iff ANY of its category memberships is enabled):
  - "✅ Done"
  - "📋 Planned"
  - "🚧 In progress"
  - "💭 Considered"
  - "Currently being tackled" — derived from CHANGELOG.md
    `[Unreleased]` + recent commit subjects.

  Plain narration bullets without a status emoji always render —
  they carry document context, not status, and can't reasonably be
  toggled off.

  All filter checkboxes have stable `objectName`s for testing.
- Body: `QSplitter(Qt::Horizontal)` with two children —
  - **Left:** `QListWidget` (`objectName "roadmap-toc"`) listing every
    `# `..`#### ` heading parsed from the markdown. Indented by two
    spaces per level above 1; level-1 headings rendered bold. The
    list is **dynamic per project** — it's rebuilt from whatever
    headings the active ROADMAP.md happens to have, no hardcoded
    section names. Selecting / clicking an entry calls
    `m_viewer->scrollToAnchor(<anchor>)`.
  - **Right:** `QTextBrowser` (read-only, rich-text). `QTextBrowser`
    instead of plain `QTextEdit` for `scrollToAnchor` support; links
    are *not* navigable (`setOpenLinks(false)` /
    `setOpenExternalLinks(false)`) so a stray markdown link can't
    replace the document. Same scroll-preservation shape as
    `ReviewChangesDialog` / `ClaudeBgTasksDialog`.
- Bottom row: `QDialogButtonBox::Close` only. The Close button is
  wired *both* via the role-based `rejected()` signal AND via a
  direct `clicked()` connection on the button itself
  (`button(QDialogButtonBox::Close)`). Some Qt 6 builds were observed
  to leave `Close` non-functional through the role-based path alone;
  the direct connection is the authoritative fix and the role-based
  one is a belt-and-braces fallback. The Close button has
  `objectName "roadmap-close-button"` for testing.

### Parsing & rendering

- Pure helper `RoadmapDialog::renderHtml(const QString &markdownText,
  Filter f, const QSet<QString> &currentBullets) → QString`. Pure +
  static so tests can drive it without spinning a Qt widget.
- Pure helper `RoadmapDialog::extractToc(const QString &markdownText)
  → QVector<TocEntry>`. Walk shape mirrors renderHtml's heading
  detection, so the N-th entry's `anchor` always equals the anchor
  emitted before the N-th heading in the rendered HTML
  (`roadmap-toc-N`). Both helpers are pure, static, and independent
  of any QWidget — tests drive them without an event loop.
- Walk the file line-by-line:
  - `^# ` → `<a name="roadmap-toc-N"></a><h1>`. `^## ` → likewise
    `<h2>`. `^### ` → `<h3>`. `^#### ` → `<h4>`. Always emitted
    regardless of filters so document structure stays intact, and
    the anchor is always present so the TOC sidebar can scroll.
  - `^\| ` (table rows) — pass through verbatim, wrapped in
    `<pre style="font-family:monospace">` since pixel-aligned ASCII
    tables don't reflow well inside the QTextBrowser viewer.
  - `^- ` bullet → leaf bullet. Inspect the first non-whitespace token
    after `- ` for a status emoji. Map:
      - `✅` → Done. Filtered out when `Done` checkbox is unchecked.
      - `📋` / `🚧` / `💭` → Planned/In-progress/Considered. Filtered
        out when `Planned` is unchecked.
      - any other (no emoji, or a non-status emoji like `🔥`) → always
        kept; treated as document narration.
    Sub-content (continuation lines starting with two spaces or a
    blank-then-`  `) attaches to the parent bullet — filter-out drops
    the whole block.
  - Blank line → `<br>` (paragraph break).
  - Other lines → wrap as `<p>`.
- Backtick `code` segments: convert with monospace style (no full
  CommonMark parser — backticks are the only inline markdown the
  ROADMAP uses outside list bullets, and they pass-through cleanly
  via a regex `\`(.+?)\`` → `<code>\1</code>`).

### Current-work highlight

A bullet is "current work" iff its leaf line contains a substring
that matches *any* line in the in-flight signal set, normalised
(lowercase, `-`/`_` collapsed to space, stripped of punctuation,
boundary-trimmed).

Signal set is built from two sources:

1. **`CHANGELOG.md` `[Unreleased]` block.** Read the file from the
   same project root as ROADMAP.md. Extract everything between
   `^## \[Unreleased\]` and the next `^## ` line. Each non-empty
   markdown bullet under it (`^- ` or `^  - `) contributes its first
   80 characters as a signal phrase.
2. **Recent commit subjects.** `git log --oneline -n 5
   --format=%s` from the project root. Skip subjects whose body
   matches `^\d+\.\d+\.\d+:` (release commits) or `^Merge ` /
   `^Revert ` (mechanical). Keep subject up to first `—` /  `:` so
   long descriptive titles get the meaningful slice.

"Currently being tackled" is a **peer category** filter, treated
the same way as the four emoji categories: a bullet renders iff
ANY of its enabled category memberships matches (inclusive OR).
A current-work bullet whose emoji category is filtered off still
renders if "Currently being tackled" is checked, and vice versa.

The yellow left-border highlight (4 px, ToolUse palette `#E5C24A`)
is applied to every rendered bullet whose payload matches a signal
phrase — independent of which checkbox kept the bullet visible.
A bullet that matches no signal phrase is *not* a current-work
candidate. If the signal set is empty (Unreleased block absent, no
recent commits), no highlights are shown — the feature is silent
when there's nothing to point at.

### Live update

`QFileSystemWatcher` on the canonical roadmap path *and* the local
`CHANGELOG.md` (if present). 200 ms debounce timer (matches the
0.7.37 / 0.7.38 dialog pattern). Reuses the same scroll-preservation
shape:

- `m_lastHtml` (`std::shared_ptr<QString>`) holds the last rendered
  HTML; identical regenerated HTML short-circuits before `setHtml`.
- Capture vbar position before `setHtml`, restore with
  `qMin(saved, vbar->maximum())` after.
- "Was at bottom" pin: if the user was within 4 px of the maximum
  scroll position before refresh, snap to the new maximum after.

### Theme

Body text colour, link colour, h-tag colour pulled from
`Themes::byName(currentTheme)`. The current-work yellow border is
fixed (`ClaudeTabIndicator::color(Glyph::ToolUse)`) since it must
read as "this is the active item" regardless of theme — same
rationale as the dot palette.

## Out of scope

- Editing the roadmap from the dialog. Read-only.
- Markdown linting / structure warnings. The dialog renders whatever
  the file contains.
- Watching commits for live current-work updates. The signal set is
  recomputed on dialog open + on `ROADMAP.md` / `CHANGELOG.md` change
  events; new commits don't trigger a refresh until one of those two
  files changes (cheap and good enough).
- Per-tab roadmap dialogs. One dialog per `MainWindow` (raised on
  re-click).
- Cross-tab coordination. Each `MainWindow` has its own button +
  dialog scoped to its active tab.

## Architectural invariants enforced by tests

`tests/features/roadmap_viewer/test_roadmap_viewer.cpp` —
source-grep + pure-helper harness, no full Qt widget tree.

- INV-1 `RoadmapDialog::renderHtml` exists as a static method on
  `RoadmapDialog` (header source-grep).
- INV-2 Filter behaviour: feeding `renderHtml` a multi-bullet input
  with one of each emoji (`✅ shipped`, `📋 planned`, `🚧 in flight`,
  `💭 considered`) plus a plain narrative bullet — toggling each
  category checkbox off drops only that category's line and keeps
  the rest; toggling all category bits off keeps only the plain
  narrative bullet (Other category is always rendered).
- INV-3 Status emojis + filter bits recognised: source-grep on the
  renderer for the four ROADMAP-legend emojis (`✅`, `📋`, `🚧`, `💭`)
  so a future emoji change doesn't silently misclassify lines, plus
  the five filter bits (`ShowDone`, `ShowPlanned`, `ShowInProgress`,
  `ShowConsidered`, `ShowCurrent`) — four map to emoji categories
  and the fifth is the CHANGELOG-derived "currently being tackled"
  signal.
- INV-4 Current-work highlight: rendering with a `currentBullets` set
  containing the substring "state-dot palette" against an input
  containing a bullet `- ✅ **State-dot palette**…` produces output
  containing the highlight CSS marker (e.g. `border-left: 4px solid
  #E5C24A`).
- INV-5 Empty signal set means no highlight: same input with empty
  `currentBullets` produces output that does NOT contain
  `border-left: 4px solid`.
- INV-6 Button hide-when-no-roadmap: `MainWindow::refreshRoadmapButton`
  source-grep — must contain a path-probe before any
  `m_roadmapBtn->show()` call, must `hide()` on the absence branch.
- INV-7 Live-update reuse: `RoadmapDialog::rebuild` / paint method
  source-grep contains the scroll-preservation triple
  (`maximum`, `qMin`, capture-before-setHtml).
- INV-8 Case-insensitive filename match: button-probe source contains
  `Qt::CaseInsensitive` (via `QDir::entryList` filter or
  `compare(..., Qt::CaseInsensitive)`).
- INV-9 Wired into MainWindow: `mainwindow.cpp` constructs an
  `m_roadmapBtn` and wires its `clicked` signal to a slot that
  instantiates `RoadmapDialog`.
- INV-10 Tab-switch refresh: `refreshStatusBarForActiveTab` calls
  `refreshRoadmapButton()`.
- INV-11 `extractToc` returns the headings in document order with
  matching levels, raw text, and anchor names of the form
  `roadmap-toc-N` — verified against a known multi-level input.
- INV-12 `renderHtml` emits an `<a name="roadmap-toc-N">` anchor
  *before* each heading element, so `QTextBrowser::scrollToAnchor`
  can position the viewer at any TOC entry.
- INV-13 The dialog wires a TOC list widget (`m_toc` + objectName
  `roadmap-toc`) into a `QSplitter` next to the viewer, and connects
  list-item activation to `scrollToAnchor` (source-grep on
  `roadmapdialog.cpp`).
- INV-14 The Close button is connected directly via
  `QAbstractButton::clicked` (not only the role-based `rejected`
  path) — guards against the Qt 6 quirk where `QDialogButtonBox::
  rejected` was observed not firing for the standard `Close` button
  (source-grep for the `closeBtn`/`QAbstractButton::clicked`
  combination).

## Rationale

The ROADMAP.md is the canonical record of where the project is
heading and where it's been. Today the only way to consult it is to
type `cat ROADMAP.md` in a terminal — which (a) defeats the
ant-terminal's-own-features story, and (b) doesn't surface "what's
in progress now" without scanning 2 000+ lines.

A status-bar button + filterable dialog turns the document into a
first-class navigation surface, the same way `Background Tasks` and
`Review Changes` did for their respective signals. Re-using the
0.7.37 / 0.7.38 scroll-preservation pattern keeps the dialog's live-
update UX consistent with its siblings.

Visibility gating on `ROADMAP.md` presence keeps the chrome clean for
terminals running outside any project root — the user's morning
"open terminal in `/tmp` to scratch" session shouldn't show a Roadmap
button pointing at nothing.
