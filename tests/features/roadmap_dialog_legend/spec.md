# roadmap_dialog_legend — the migrated project's own status legend

**Owner:** ANTS-3793 § 2.3
**Bundle:** `test_dialogs` (see "Why not `roadmap_read_seam/`" below)

## Problem

`RoadmapDialog` holds the four status emojis and their labels as compile-time
constants (`kStatusLabels` in `src/roadmapdialog.cpp`, a
`kEmojiDone`/`kEmojiInProgress`/`kEmojiPlanned`/`kEmojiConsidered` →
`QT_TR_NOOP` table). Once a project is served by the store, its legend is
per-project data — `project.legend_text`, written by the migration and rendered
by `roadmaprender.cpp` — and the dialog should show *that* project's vocabulary
rather than this one's.

The trap the third row guards is the reason this file exists. Written as the
flat rule "a project with no stored legend shows none", § 2.3 would silently
delete the legend from every **markdown-served** project — which is most of
them for as long as the rollout takes — a user-visible regression arriving as a
side effect of a read-path change.

## Invariants

- **INV-1 (Inv2Legend, row 1)** — a migrated project **with** a stored legend
  renders that legend's labels. *Breaks when:* the renderer keeps using
  `statusAccessibleLabel()` on the store path, or the store's lifecycle-word
  keys (`"planned"`, `"shipped"`, …) are used unmapped against records keyed by
  status emoji.
- **INV-2 (Inv2Legend, row 2)** — a migrated project with **no** stored legend
  renders **no** label. *Breaks when:* the compile-time table is used as a
  default on the store path — which would show this project's vocabulary for
  another project's statuses, the thing a per-project legend exists to prevent.
- **INV-3 (Inv2Legend, row 3)** — an **unmigrated** project still renders
  today's compile-time labels, unchanged. *Breaks when:* rows 1 and 2 are
  implemented by replacing the table rather than by branching on which backend
  answered.
- **INV-4** — `RoadmapSource::legendByEmoji()` skips a lifecycle word with no
  glyph (`dropped`) instead of inserting an empty key. *Breaks when:* the
  word→emoji map is re-implemented in the dialog instead of reusing
  `RoadmapRender::emojiFor()`, or the empty return is not checked.

## Why not `roadmap_read_seam/`

ANTS-3793 § 6 lists `Inv2Legend` beside the other five cases in
`tests/features/roadmap_read_seam/`, which compiles into **`test_core`**. That
bundle does not link `ants_dialogs_lib`, so a case there cannot reach
`RoadmapDialog::renderCardsHtml` at all. The case keeps its spec'd name and
moves to the bundle that can run it; the other five are unaffected.

## Test scope

Behavioural, against a real on-disk store (`Access::Interactive`, explicit
path — never a default-constructed `RoadmapStore`, which resolves the
developer's real store under `XDG_DATA_HOME`) plus
`RoadmapDialog::renderCardsHtml`, which is static and takes its records through
`CardRenderOptions` exactly as the dialog supplies them.
