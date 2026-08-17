# roadmap_toc_toggle — the contents pane hides, stays hidden, and costs nothing while hidden

ANTS-4415. Contract for the roadmap dialog's table-of-contents pane becoming
collapsible, and for the render skipping it while it is not shown.

The pane is `m_toc`, a `QListWidget` at index 0 of `roadmap-splitter`; the
viewer is index 1. Before this change the pane was always visible and always
repopulated.

## Why the skip is part of the contract and not an optimisation

`rebuild()` clears the pane and repopulates it on **every** render — not only
on open, but on every filter toggle and every debounced search keystroke. It
walks the markdown through `extractToc()` and constructs one
`QListWidgetItem` per entry (245 on the Ants roadmap). A hidden pane has no
observer, so doing that work is indistinguishable from not doing it except in
time.

That makes "hidden" a claim the render has to honour, which is why it is an
invariant here rather than a note. A toggle that hid the widget and left the
walk running would look identical from the outside and would be wrong.

## Invariants under test

- **INV-1.** The toggle exists as a `QToolButton` named
  `roadmap-toc-toggle-button`, is checkable, and is tab-reachable
  (`Qt::StrongFocus`). It is a view control, so it does not narrow the list and
  must not be confused with the ANTS-4412 filter buttons beside it.

- **INV-2.** Its checked state and the pane's visibility agree in both
  directions. Checked ⟺ `m_toc->isVisible()`. The button is checkable rather
  than label-swapping precisely so the control cannot disagree with the thing
  it controls.

- **INV-3.** Unchecking hides the pane; re-checking shows it again **and
  repopulates it**. Because INV-4 skips the walk while hidden, a pane restored
  without a re-render would show whatever the last visible render left, or
  nothing at all if the dialog opened hidden.

- **INV-4.** While the pane is hidden, `rebuild()` performs no TOC work: the
  pane's item count does not change across a render triggered by a filter
  change. Asserted by mutating a filter with the pane hidden and confirming
  the count is untouched, then showing it and confirming it fills.

- **INV-5.** The choice persists. `Config::roadmapTocVisible()` is written on
  toggle, and a dialog constructed against a `Config` whose key is false opens
  with the pane hidden. Missing key → **true** (the pane's shipped state); an
  absent key means nobody has chosen, and hiding a pane the user never asked
  to lose is the worse of the two wrong answers.

- **INV-6.** The splitter can collapse the pane by drag as well as by button —
  `setCollapsible(0, true)`. `m_toc` carries a 180px minimum width, which
  without this would stop a drag short of zero and leave the handle and the
  button disagreeing about whether the pane can go away.

## Test shape

`RoadmapDialog` over a throwaway roadmap in a `QTemporaryDir`, driven through
the real widgets by `objectName` — the same shape as
`tests/features/roadmap_filter_bar/`.

`XdgGuard::setTestMode(false)` is load-bearing and deliberate: test mode makes
`QStandardPaths` ignore `XDG_CONFIG_HOME` in favour of one shared per-binary
location, so a sibling test in this bundle that persists dialog state has its
`Config` restored into this dialog. Measured on
`tests/features/roadmap_filter_bar/` — the defaults asserted there came back as
whatever had run before.
