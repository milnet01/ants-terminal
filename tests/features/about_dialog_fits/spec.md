# About dialog fits its text (ANTS-5390)

The About dialog clipped its body at its default or saved size (user
screenshot 2026-09-26). The body grows when the MCP reload notice shows.

## Invariants

- **INV-1** — the About Ants dialog is at least 560 px wide, and a smaller
  requested size is clamped up.
- **INV-2** — at any size the dialog can take, its body label is as tall as
  its wrapped text needs at its current width, so no line is cut off.

*Test:* `test_about_dialog_fits.cpp` opens the real dialog, asks for a size
far below the text, and checks both.
