# Feature spec: a refused session never costs the user a good file (ANTS-5031)

`SessionManager::restore` refuses a blob that is too large, fails its
checksum, comes from a newer version, or ends partway through. Three
defects turned a refusal into data loss.

1. `MainWindow::restoreSessions` ignores `loadSession`'s result and opens
   the tab under the same id, so the next 30-second save overwrote the
   refused file.
2. `restore` resized the grid and pushed scrollback before it had
   validated the stream, so a refusal left the tab part-restored.
3. `serialize` had no size cap, so a large scrollback could write a blob
   `restore` refuses.

## Invariants

- **INV-A — a refused restore leaves the grid untouched.** A blob whose
  stream ends partway through makes `restore` return false, and the grid
  keeps its dimensions and an empty scrollback.
- **INV-B — a refused file is kept.** `loadSession` on a file `restore`
  refuses copies it aside through `rotateCorruptFileAside`, so the copy
  holds the refused bytes.
- **INV-C — save writes only what restore accepts.** `serialize` takes a
  cap on the uncompressed stream and on the file. It keeps the newest
  scrollback lines that fit, so its output is within both caps and
  `restore` accepts it. The defaults are `MAX_RESTORE_RAW_BYTES` and
  `MAX_RESTORE_FILE_BYTES`, the limits `restore` enforces.
- **INV-D — a copy kept aside is swept by age (ANTS-6508).**
  `cleanupOldSessions` removes a `session_*.dat.corrupt-*` copy older than
  its age limit, as it does a session file, and keeps a recent one.
- **INV-E — a screen larger than its header is cropped, not refused
  (ANTS-6593).** A stream whose screen lines are wider, and whose screen
  rows are more, than its header's grid restores at the header's size. The
  extra cells and rows are read and dropped, so the fields after the
  screen still restore.

## Test scope

Behavioural against `SessionManager` and `TerminalGrid`, headless. INV-A
builds a legacy blob with no checksum, so the cut is found only while
parsing. INV-B isolates the sessions directory with `QStandardPaths` test
mode. INV-C passes small caps, so the test stays small. INV-E saves a
grid, then rewrites the legacy blob's header to a smaller grid.
