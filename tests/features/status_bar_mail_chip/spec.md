# Status-bar unread-mail chip (ANTS-5620)

User request, 2026-10-02: show, per tab, whether that session has unread
`session_message` mail. ANTS-5619 tells the Claude session itself; this
tells the person looking at the terminal.

## Contract

- **INV-1 — project lookup.** `RoadmapStore::projectIdContaining(path)`
  returns the id of the registered project whose root is `path` or the
  nearest directory above it. A path under no registered root returns
  nullopt. It never registers anything.
- **INV-2 — count.** The chip's number is `mailSummaryFor()`'s unread
  count for that project: acked mail does not count, and mail this project
  sent does not count.
- **INV-3 — face.** `ClaudeStatusBarController::refreshMailChip()` shows
  `✉ N unread` for the focused tab's project and hides the chip at zero,
  when the tab has no cwd, or when the cwd is in no registered project.
  Its tooltip names the senders. It sets an objectName and an accessible
  name.
- **INV-4 — refresh.** It runs on the 2 s status timer and on both arms of
  `MainWindow::refreshStatusBarForActiveTab` (with a terminal, and without
  one), which every tab switch runs, per `claude_status_bar/spec.md` § D
  for a state widget.
- **INV-5 — cost.** It re-queries the store only when the focused cwd or
  the store's files (`roadmap.sqlite`, `roadmap.sqlite-wal`) changed since
  its last query. It opens its own store connection through
  `RoadmapSource::storeFor()`, on the GUI thread, and never creates a
  store that is not there.

## Tests

- `StatusBarMailChip.Inv1ProjectIdContaining` — temp store, one registered
  root: the root and a subdirectory resolve to it; a sibling directory and
  `/` do not.
- `StatusBarMailChip.Inv2CountsOnlyUnreadInbox` — two messages to the
  project, one acked, one sent by it: the unread count is 1.
- `StatusBarMailChip.Inv3To5Wiring` — source scrape: the chip's objectName,
  the hide-at-zero branch, the face text, the signature short-circuit, the
  `storeFor` open, the timer connect, and two `refreshMailChip` calls in
  `refreshStatusBarForActiveTab`.

All three fail against the tree before ANTS-5620.

## Reload story

GUI code: the chip appears after the next terminal relaunch, which a
status-bar widget cannot avoid. Once running, the count is re-read from
the store on each change, with nothing further to reload.
