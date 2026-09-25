# The roadmap dialog's title names its source (ANTS-5368)

The title read "Roadmap — ROADMAP.md" even on a store-served project, where
the file is only the store's rendered copy. `RoadmapDialog::rebuild()`
already records the backend of each fresh read in `m_source.fromStore`; the
title follows it.

## Invariants

- **INV-1** — on a project the store does not serve, the title is
  "Roadmap — from ROADMAP.md" (the file's basename).
  *Test:* `Inv1FileProjectNamesTheFile`.
- **INV-2** — on a project the store serves, the title is
  "Roadmap — from the roadmap store".
  *Test:* `Inv2StoreProjectNamesTheStore`.

Both fail against the pre-fix title, which named the file in both cases and
had no "from".

## Build

Compiled into `test_dialogs`, the bundle RoadmapDialog links in. Both cases
redirect XDG_CONFIG_HOME and XDG_DATA_HOME into a temp dir, so the store is a
sandbox, never the machine's real one.
