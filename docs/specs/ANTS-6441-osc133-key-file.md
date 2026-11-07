# ANTS-6441 — keep the OSC 133 key in a file, not in every process's environment

**Status:** accepted (2026-11-07).
**Kind:** security.
**Source:** ROADMAP ANTS-6441 (review-code 2026-11-06, lane 02-terminal-grid; design decided by the user 2026-11-06).
**Pairs with:** ANTS-6444, the shell-integration README's false claim, closed by § 4.4.

**Layman:** The secret that lets Ants tell your real prompt from a fake one moves into a private file, so programs you run no longer inherit it.

## 1. Goal

The OSC 133 HMAC key lives in a private key file. The terminal and both
shell hooks read it from there, and no process the shell starts inherits it.

## 2. Problem

The key reaches the terminal and the hooks as `$ANTS_OSC133_KEY`, exported
from `~/.profile` or `~/.zshenv`. Neither hook un-exports it, so every
program the shell runs inherits the key and can sign a forged OSC 133
marker. `packaging/shell-integration/README.md` claims inner processes do
not know the key (ANTS-6444); today that is false.

## 3. Scope decisions (agreed with the user)

The user chose, on 2026-11-06:

- A key **file**, not un-exporting alone, and not a README change alone.
- The terminal reads the file for each new tab, so a new key reaches new
  tabs without a relaunch.
- `$ANTS_OSC133_KEY` stays honoured for old setups, and the hooks
  un-export it.
- Nested shells read the file too, so they keep their command blocks.

## 4. Surface

### 4.1 The key file

`$XDG_CONFIG_HOME/ants-terminal/osc133.key`, with `XDG_CONFIG_HOME`
defaulting to `~/.config`. In C++ the directory is
`QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)`,
which honours the same variable. It holds the key on its first line; a
trailing newline and surrounding whitespace are ignored.

### 4.2 Which key wins

The terminal and both hooks apply one rule: the key file's first line when
it is non-empty, otherwise a non-empty `$ANTS_OSC133_KEY`, otherwise no key
and the verifier is off.

### 4.3 The terminal

`TerminalGrid`'s constructor reads the key through one function,
`osc133KeyFromSetup()`, which applies § 4.2. Each new tab constructs a grid,
so it reads the file afresh. A reset (`ESC c`) keeps the tab's key, as
`tests/features/ris_preserves_callbacks/spec.md` already requires.

### 4.4 The hooks

`ants-osc133.bash` and `ants-osc133.zsh`, before their other early returns:

1. Read the key per § 4.2 into `__ants_osc133_key`, a shell variable that
   is not exported (bash: `export -n` after the assignment, since `set -a`
   exports it; zsh: `typeset +x`).
2. Un-export `$ANTS_OSC133_KEY` where it is set (bash `export -n`, zsh
   `typeset +x`). The shell keeps the value; its children no longer get it.
3. Return early when `__ants_osc133_key` is empty, as they now do for an
   empty `$ANTS_OSC133_KEY`.

Signing pipes `__ants_osc133_key`, not `$ANTS_OSC133_KEY`, into the signer.
The hooks must still run under `nounset` (`tests/features/osc133_nounset`).

The README and the hooks' header comments say what the key now defends
against: forged markers in printed output (`cat`, remote output). Programs
the user runs as themselves can read the key file, and the HMAC does not
stop them (ANTS-6444).

### 4.5 The setup wizard

This amends `docs/specs/ANTS-5558-welcome-dialog.md` § 2.6.
`installShellIntegration()`:

- writes the key to the key file, creating its directory, with the file at
  mode 0600;
- takes the key from the key file, else from an old `~/.profile` /
  `~/.zshenv` block, else from `QRandomGenerator::system()`, so a reinstall
  never changes a working key;
- removes the old marked block from `~/.profile` / `~/.zshenv`, keeping
  every byte outside it;
- writes the `~/.bashrc` / `~/.zshrc` source block as before.

`shellIntegrationStatus()` is `Installed` when the key file holds a key, the
rc block is present and the sourced script exists; `Partial` when any of
those, or an old profile block, is present; `Missing` otherwise. The
preview names the key file without showing the key. Success says new tabs
pick up the key and the source line takes effect in a new shell.

## 5. Invariants

- **INV-1 — the file wins.** With the key file holding K1 and
  `$ANTS_OSC133_KEY` set to K2, a new `TerminalGrid` applies a marker signed
  with K1 and rejects one signed with K2. Broken by reading the environment
  first. *Test:* `Osc133KeyFile.Inv1FileWinsOverEnv`.
- **INV-2 — the environment is the fallback.** With no key file, or an empty
  one, a new grid takes `$ANTS_OSC133_KEY`; with neither, the verifier is
  off. Broken by dropping old setups. *Test:* `Osc133KeyFile.Inv2EnvFallback`.
- **INV-3 — each new tab reads the file.** A grid built after the file
  changes uses the new key; one built before keeps the old key. Broken by
  caching the key per process. *Test:* `Osc133KeyFile.Inv3NewGridReadsFile`.
- **INV-4 — the hooks keep the key out of children.** After sourcing either
  hook with `$ANTS_OSC133_KEY` exported and a key file present, a child
  process sees neither `ANTS_OSC133_KEY` nor `__ants_osc133_key`, and the
  emitted marker's `ahmac` is the HMAC under the file's key. Broken by an
  exported key variable. *Test:* `test_osc133_key_file.sh`.
- **INV-5 — the hooks fall back to the environment.** With no key file and
  `$ANTS_OSC133_KEY` set, the hook signs with it and still un-exports it.
  *Test:* `test_osc133_key_file.sh`.
- **INV-6 — the wizard writes the file and keeps the key.** After
  `installShellIntegration()` the key file holds 64 hex digits at mode 0600,
  the profile holds no marked block, and a key from an old profile block is
  the key written. *Test:* `test_claude_setup.cpp` (ANTS-5558 INV-8,
  amended).

## 6. Failure modes

- **Unreadable key file.** Treated as absent: the environment fallback
  applies. Nothing is logged, since the terminal reads it for every tab.
- **Old setup, launched from a shell that has un-exported the key.** The
  terminal gets no key and its verifier is off, while a login shell in a
  tab may still sign. Markers are then accepted unchecked, as with no
  integration. Reinstalling from the wizard moves the key into the file.

## 7. Tests

- `tests/features/osc133_key_file/test_osc133_key_file.cpp` (bundle
  `test_vt`) — INV-1, INV-2, INV-3. The bundle main already points
  `XDG_CONFIG_HOME` at a temporary directory, so no test reads the user's
  real key file.
- `tests/features/osc133_key_file/test_osc133_key_file.sh` — INV-4,
  INV-5, in bash and in zsh; the zsh half is skipped, with a message, when
  zsh is not installed.
- `tests/features/welcome_dialog/test_claude_setup.cpp` and
  `test_welcome_dialog.cpp` — INV-6, replacing their `~/.profile` key
  assertions.
- `tests/features/osc133_hmac_verification` Test 4 — now asserts that
  `osc133KeyFromSetup()` is the only source of the production key.

## 8. Alternatives considered (and rejected)

- **Un-export only.** Rejected by the user: nested shells then lose the key
  and their command blocks.
- **Hand the key over an inherited file descriptor.** Rejected: every
  nested shell would need the descriptor passed on, and a key file serves
  nested shells with no plumbing.
- **README change only.** Rejected by the user: it documents the leak
  instead of closing it.

## 9. Out of scope

- Hiding the key from programs running as the user. They can read the file;
  § 4.4's README text says so.
- Flatpak, where the sandbox has its own `XDG_CONFIG_HOME`. Unverified.
- Removing `$ANTS_OSC133_KEY` support.

## 10. What checks this

| Rule | What catches a breach |
|---|---|
| File before environment | INV-1, INV-4 |
| Environment fallback | INV-2, INV-5 |
| Fresh read per tab | INV-3 |
| No child inherits the key | INV-4 |
| Wizard writes the file, keeps the key, drops the profile block | INV-6 |
| One production source of the key | `osc133_hmac_verification` Test 4 |
| README and hook comments state the real defence | **nothing** — prose, caught only by review |

## 11. Cross-doc impact

- `docs/specs/ANTS-5558-welcome-dialog.md` § 2.6 — amended to § 4.5.
- `tests/features/osc133_hmac_verification/spec.md` — the key source and
  the false inner-process claim.
- `packaging/shell-integration/README.md` and both hooks' header comments.
- `CHANGELOG.md` `[Unreleased]`.

## 12. Cold-eyes loop log

| Date | Loop | Result |
|---|---|---|
| 2026-11-07 | none | Not reviewed: the user's 2026-11-06 decision on ANTS-6441 sets no `review-contract` gate unless the spec proves expensive. Its contract is one path, one format and one precedence rule, each cheap to change. |

## 13. Reload

A new key file reaches each new tab, which reads it at construction, and
each new shell, whose hook reads it when sourced. Open tabs and their
shells keep the key they started with, so the two stay matched. No
relaunch. The feature holds no running state beyond each tab's key, so it
has no stop/start control. It runs in the terminal process because the
verifier lives in `TerminalGrid`.
