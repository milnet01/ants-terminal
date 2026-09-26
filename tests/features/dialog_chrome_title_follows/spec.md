# Themed title bar follows a dialog's title (ANTS-5391)

`DialogChrome::install()` draws its own title bar and copied the dialog's
title into it once. A dialog that retitled itself later kept showing the old
title: the roadmap dialog names its source only after reading it (ANTS-5368),
so a store-backed project still showed `Roadmap — ROADMAP.md` (user
screenshot 2026-09-26).

## Invariants

- **INV-1** — after `setWindowTitle()` on a dialog that has the chrome, the
  title bar shows the new title.

*Test:* `test_dialog_chrome_title_follows.cpp`.
