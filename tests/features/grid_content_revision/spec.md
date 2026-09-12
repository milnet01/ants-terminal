# Feature: grid content revision

## Contract

`TerminalGrid::contentRevision()` is a monotonic counter that changes
whenever anything the session blob records could have changed, and does
not change while the grid is idle.

The session blob records the screen cells, the scrollback, the grid
dimensions, the cursor position and the window title. A caller may
compare the counter against a previously observed value and, when it is
equal, conclude the blob it would write is the one already written.

## Rationale

`MainWindow::saveAllSessions` runs on a 30-second timer and called
`SessionManager::saveSession` for every tab unconditionally. That
serialises every cell of the grid including scrollback, compresses,
hashes, writes and fsyncs it, on the GUI thread, whether or not the tab
had changed. At the default scrollback depth that is tens of megabytes
per tab; at the maximum it is far more. Most tabs are idle most of the
time, so most of that work produced a file identical to the one already
on disk.

The counter is the skip test. Its bias is deliberate: it is bumped when
mutable access is handed out, not when a write is observed, so a caller
that takes a reference and writes nothing still bumps it. Over-counting
costs one redundant save. Under-counting would drop a save the user
needed, so the counter must never miss a mutation.

The per-line dirty flags are not a substitute. They gate the span
caches, and the paint loop redraws every visible row regardless, so they
are not a complete record of what has been mutated.

## Invariant

1. A grid that receives no input does not change its revision, however
   many times it is read.
2. Printing a character changes it.
3. Pushing a scrollback line changes it.
4. Resizing changes it.
5. Setting the window title changes it.
6. Moving the cursor changes it.
7. Clearing the screen changes it.

## Scope

### In scope
- The counter's stability while idle and its response to each mutation
  path named above.

### Out of scope
- The value of the counter, and the size of any increment. Only change
  and non-change are contractual.
- `MainWindow`'s use of the counter, which needs a window and is
  covered by the app's own behaviour rather than by this test.
