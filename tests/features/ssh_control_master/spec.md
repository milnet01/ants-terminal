# Feature: SSH ControlMaster auto-multiplexing in the bookmark dialog

## Contract

`SshBookmark::toSshCommand(bool controlMaster)` builds the shell
command invoked when the user hits **Connect** in the SSH manager.
When `controlMaster=true`, the command MUST carry three `-o` options
so OpenSSH reuses a live session for repeat connects to the same
host; when `false` (the default), the command MUST be byte-identical
to the pre-0.7.1 output.

| Scenario | `-o ControlMaster=auto` | `-o ControlPath=…` | `-o ControlPersist=10m` |
|----------|------------------------|--------------------|-------------------------|
| `toSshCommand()` (default)    | absent  | absent  | absent  |
| `toSshCommand(false)`         | absent  | absent  | absent  |
| `toSshCommand(true)`          | present | present | present |

When the flags are emitted they MUST appear **before** the final
`[user@]host` destination argument. That matches `ssh(1)`'s
convention of global options preceding the destination.

## Invariants

- **INV-1** — default and `toSshCommand(false)` produce zero
  `Control*` tokens. Existing bookmarks that predate this feature
  (or users who explicitly disable multiplexing via
  `ssh_control_master=false` in `config.json`) see the legacy
  command verbatim.
- **INV-2** — `toSshCommand(true)` emits `-o ControlMaster=auto`,
  `-o ControlPath=<resolved $HOME>/.ssh/cm-%r@%h:%p`, and
  `-o ControlPersist=10m`.
- **INV-3** — the ControlPath MUST resolve `$HOME` in-process via
  `QDir::homePath()`. A literal `~/` prefix would be carried
  verbatim into the ssh arg; bash expands `foo=~/…` in command
  args but dash/posix `sh` do not, and OpenSSH itself never does
  tilde expansion — so relying on the shell is non-portable.
- **INV-4** — the `%r@%h:%p` tokens MUST survive shell quoting
  intact. These are OpenSSH ControlPath substitutions (`%r` = remote
  user, `%h` = hostname, `%p` = port), not shell metacharacters, so
  the existing `shellQuote()` allow-set (which treats `%`, `@`, `:`
  as non-special) correctly leaves them alone.
- **INV-5** — the three `-o` options MUST precede the destination
  `[user@]host` argument. SSH accepts options in either order in
  practice, but all reference terminals (OpenSSH's own examples,
  tmux, mosh, kitty) emit them first. Placing them after `host`
  looks wrong on inspection.
- **INV-6** — `controlMaster=true` MUST coexist with the pre-
  existing `-p <port>`, `-i <identity>`, and `extraArgs` options
  without interfering. Regression guard against accidentally
  swallowing or reordering those legacy args.

## Rationale

Every modern terminal with an SSH integration pushes ControlMaster:
kitty's `ssh` kitten
([docs](https://sw.kovidgoyal.net/kitty/kittens/ssh/)), Warp's SSH
blocks, iTerm2's SSH profiles. The cost is nil (a 10-minute unix
socket in `~/.ssh/`), the payoff is second-and-subsequent connects
to the same host skip the full auth handshake — typically 200–500 ms
over LAN, multiple seconds over WAN with ssh-agent + 2FA.

The feature is opt-in via `config.json` (`ssh_control_master`,
default `true`) because some sites forbid ControlMaster by policy
(lingering sockets retain cached credentials; `ControlPersist` means
the socket outlives the first tab's window close). Default-on
matches kitty's precedent; users who disable it get byte-identical
legacy behaviour.

## Scope

### In scope
- The contract of `SshBookmark::toSshCommand()` under both flag
  values, covering all three option presence/absence invariants,
  `$HOME` resolution, token preservation, positional ordering, and
  coexistence with legacy flags.

### Out of scope
- The `Config::sshControlMaster()` getter plumbing (covered by
  reading the config.cpp default value — straightforward).
- Live socket lifecycle (requires an actual OpenSSH binary +
  reachable host; covered by the manual acceptance walkthrough in
  `packaging/README.md`).
- The Settings-dialog toggle (pending; the roadmap item lands the
  command plumbing first, UI toggle in a follow-up).

## Test strategy

Headless. Construct `SshBookmark` by hand, call `toSshCommand()`
with both flag values, and assert token presence / absence /
ordering on the resulting QString. No QApplication is required —
`toSshCommand()` uses only `QString`, `QStringList`,
`QRegularExpression`, and `QDir::homePath()`, none of which depend
on a running event loop.

The test links `src/sshdialog.cpp` directly (where the `shellQuote`
helper lives as a translation-unit static). It links
`Qt6::Widgets` because `sshdialog.cpp` includes `QDialog` for its
`SshDialog` class — even though we never construct one.
