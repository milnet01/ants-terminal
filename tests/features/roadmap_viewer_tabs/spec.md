# Roadmap dialog — faceted tabs + search + larger window (ANTS-1100)

User request 2026-04-30 (refines the earlier 2026-04-28 ask, ANTS-1056):
"please add features to the Roadmap dialog box that you think will be useful…
also come up with a standard for roadmap.md that we can share with Claude Code
sessions." Three coordinated changes to `RoadmapDialog`, all on top of the
existing parser / renderer (no parser change — pure presentation layer):

1. **Tab strip** above the TOC — five faceted views.

   | Tab          | Filter mask                                                        | Sort                          |
   |--------------|--------------------------------------------------------------------|-------------------------------|
   | Full         | All bits set (Done + Planned + InProgress + Considered + Current)  | Document                      |
   | History      | ShowDone only                                                       | DescendingChronological       |
   | Current      | ShowInProgress \| ShowCurrent                                       | Document                      |
   | Next         | ShowPlanned only                                                    | Document                      |
   | Far Future   | ShowConsidered only                                                 | Document                      |
   | Custom       | (whatever the checkboxes say, when no preset matches)              | Document (or last-preset)     |

   Existing five filter checkboxes stay — toggling one switches to the
   "Custom" implicit tab (de-emphasised) so the user can fine-tune.

2. **Search field** above the TOC. Case-insensitive substring filter applied
   across bullet headlines + bodies, scoped to the active tab's filter.
   Live update via `QLineEdit::textChanged` → re-render. Accepts `id:NNNN`
   shorthand to match a specific `[ANTS-NNNN]` bullet regardless of
   headline content. Debounced at ~120 ms.

3. **Larger default size + persisted geometry.** Bump default from current
   900×700 to 1200×800. Persist via `Config::roadmapDialogGeometry`
   (saveGeometry / restoreGeometry round-trip — same shape as the
   `windowGeometryBase64` pattern already in Config).

## Invariants

- **INV-1** `filterFor(Preset::Full)` returns all five `Show*` bits OR'd.
- **INV-2** `filterFor(Preset::History)` returns `ShowDone` only.
- **INV-3** `filterFor(Preset::Current)` returns `ShowInProgress | ShowCurrent`.
- **INV-4** `filterFor(Preset::Next)` returns `ShowPlanned` only.
- **INV-5** `filterFor(Preset::FarFuture)` returns `ShowConsidered` only.
- **INV-6** `sortFor(History)` is `DescendingChronological`; every other
  named preset returns `Document`.
- **INV-7** The tab bar is the first widget in the dialog's vertical
  layout (above the filter checkbox row).
- **INV-8** `presetMatching(filter, sort)` returns `Custom` when the
  active filter+sort combo doesn't equal any of the five named presets.
- **INV-9** `renderHtml(..., DescendingChronological, ...)` against a
  multi-section markdown emits the sections in reverse document order.
- **INV-9b** `renderHtml(..., Document, ...)` against the same input
  preserves the authored section order — the renderer never
  re-orders sections when sort is `Document` (negative case
  guarding INV-9 against false positives).
- **INV-10** Search predicate `"OSC 8"` against a doc with two bullets
  — one mentioning `OSC 8`, one not — yields exactly one bullet in
  the rendered HTML (the one mentioning `OSC 8`).
- **INV-11** Search predicate `"id:1042"` against a doc with two
  bullets — one labelled `[ANTS-1042]` (different headline), one
  not — keeps only the `[ANTS-1042]` bullet.
- **INV-12** Dialog default size is at least 1100×720 (asserted
  numerically against the parsed `resize(W, H)` literal — not via
  string-match, so a future polish bump can't silently break the
  test); geometry is persisted/restored via
  `Config::roadmapDialogGeometry`.
- **INV-13** `sortFor(Preset::Custom)` returns `Document` — when the
  user diverges from any named preset via the checkbox row, the
  dialog inherits document order rather than silently re-sorting.

Test harness: source-grep for the structural invariants; behavioural
invariants drive `RoadmapDialog::renderHtml` / `filterFor` / `sortFor` /
`presetMatching` directly (the same pattern as roadmap_viewer).
