# Remote-control `launch` — convenience wrapper for new-tab + send-text

## Contract

Seventh and final command in the initial rc_protocol surface. Spawns
a command in a new tab — sugar for the common
`idx=$(... new-tab) && ... send-text --remote-tab $idx --remote-text $cmd$'\n'`
pattern, collapsed into one round-trip.

Request shape:

    {"cmd":"launch","cwd":"<path optional>","command":"<string required>"}

- `cwd`     — same semantics as `new-tab`: empty/absent → inherit
  from focused/current terminal.
- `command` — **required**. Empty / missing yields
  `launch: missing or empty "command" field (use new-tab if you
  want a bare shell)`. The error message hard-codes the suggestion to
  reach for `new-tab` instead — caller-friendly diagnostics over
  silent fallback.

Response shape on success:

    {"ok":true,"index":<int>}

`index` is the new tab's 0-based position — chain-friendly with
`select-window` / `send-text` / `get-text` / etc.

### Newline behaviour — the difference from `new-tab`

`launch` auto-appends `\n` to `command` if missing. `new-tab` does
not (it sends raw bytes, matching `send-text`). Two scenarios:

- `--remote-command 'echo hi'` via **`launch`** → shell receives
  `echo hi\n` → command runs.
- `--remote-command 'echo hi'` via **`new-tab`** → shell receives
  `echo hi` → command sits at prompt, awaits user-pressed Enter.

That's the convenience: `launch` "just works" for the spawn-this-and-
forget case; `new-tab` stays byte-faithful for scripts that compose
multi-line input or send control sequences.

## Invariants (source-grep)

- **INV-1** `RemoteControl::dispatch` routes `"launch"` to
  `cmdLaunch`.
- **INV-2** `cmdLaunch` requires a non-empty string `command` field;
  rejects with the documented error message that points at `new-tab`.
- **INV-3** `cmdLaunch` auto-appends `\n` to `command` when not
  already present — that's the convenience contract.
- **INV-4** `cmdLaunch` delegates to `MainWindow::newTabForRemote`
  (no parallel implementation).
- **INV-5 (cross-cutting)** `MainWindow::newTabForRemote` uses
  `sendToPty` (raw bytes) rather than `writeCommand` (auto-newline)
  for the deferred command write — keeps `new-tab`'s "caller owns
  newline" contract honest. Was `writeCommand` until 2026-04-21
  when `launch` shipped and exposed the contract drift.
- **INV-6** Client CLI: `launch` shares the `--remote-cwd` /
  `--remote-command` options with `new-tab` — the `else if (cmd ==
  "new-tab" || cmd == "launch")` branch in `main.cpp` forwards
  both fields uniformly.
