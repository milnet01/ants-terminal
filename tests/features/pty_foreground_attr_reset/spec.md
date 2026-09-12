# Feature: character attributes reset when a foreground program exits

## Contract

When a foreground program exits, the terminal clears the character
attributes it left set, so the shell prompt written next renders at the
theme's default foreground rather than inheriting the program's styling.

The decision is made from the pty's **foreground process group**, and it
fires only on the transition back to the shell:

| Previous sample | Current sample | Reset? |
|---|---|---|
| (none — first read) | anything | no |
| shell | shell | no — an idle shell |
| shell | program | no — a program starting |
| program | same program | no — a program producing output |
| program A | program B | no — neither is the shell |
| program | shell | **yes — the program exited** |
| anything | unreadable (`<= 0`) | no |
| unreadable | shell | no — it can miss, never invent |

## Rationale

Claude Code emits the dim attribute as `ESC[2m` ... `ESC[22m` around its
secondary status lines. Interrupt or kill it between the two and dim
stays set — which is correct VT behaviour, because SGR is terminal state
and not process state, so no parser fix reaches it.

On this host nothing ever clears it. openSUSE's default prompt in
`/etc/bash.bashrc` is `"${USER}@${HOST}:${PWD}> "` with no SGR escapes
at all, not even a reset. Most distributions' prompts emit one and
self-heal on the next prompt; this one never does. So every later
prompt, and everything the user types, renders dim until something
happens to emit SGR 0 or 22.

Measured: the user's Kanagawa `textPrimary` `#DCD7BA` (220,215,186)
rendered as (147,143,124) — exactly `darker(150)`, the dim path in
`TerminalWidget::paintEvent`. Reproduced in a throwaway `--e2e`
instance: plain text after an unterminated `ESC[2m` lands on
(147,143,124) with channel coverage 1.00/1.00/1.00, against
(220,215,186) for the control line before it.

This is ANTS-5130's defect class arriving by a different route. That
item fixed a truncated-SGR parse that executed a colour operand as
SGR 2; this one needs no malformed input at all.

## Invariant

`Pty::foregroundReturnedToShell(fg, shellPgid, lastSeen)` implements the
table above and updates `lastSeen` in place on every call.

- **INV-1** The first sample never fires, because `lastSeen` starts -1.
  Otherwise a terminal opening with the shell already idle would reset
  on its very first read.
- **INV-2** An idle shell (shell → shell) does not fire. Steady state
  must be silent or every read would reset.
- **INV-3** A program starting (shell → program) does not fire.
- **INV-4** A program producing output (program → same program) does not
  fire, so styling is never stripped mid-output.
- **INV-5** Two different non-shell groups (program A → program B) do
  not fire. Only a return to the shell counts.
- **INV-6** program → shell fires exactly once. A second identical
  sample afterwards does not fire again.
- **INV-7** An unreadable foreground (`fg <= 0`) never fires, and
  `lastSeen` is set to that value so the *next* sample cannot read as a
  transition. The bias is deliberate: the check may miss a transition,
  and must never invent one.
- **INV-8** `shellPgid <= 0` never fires — the child has been reaped and
  there is no shell to return to.
- **INV-9** `Pty::onReadReady` samples `tcgetpgrp` and, on a true
  verdict, emits the reset **into the data stream** ahead of the bytes
  it is about to deliver. Ordering is the reason: a separate signal
  could overtake queued parse batches, and the reset must reach the
  parser before the prompt it is meant to clean.

## Scope

### In scope
- The transition state machine, every row of the table.
- That the wiring in `onReadReady` exists and injects ahead of the data.

### Out of scope
- Whether the reset reaches the screen. That needs a real pty, a real
  shell and a real external program; it is verified by the `--e2e`
  pixel measurement recorded under Rationale, not here.
- `printf` and other shell **builtins**. They run in the shell's own
  process group, so no transition occurs and nothing is reset. This is
  a real limit of the mechanism, not a defect: the reported case is an
  external program.
- A program that deliberately sets attributes and exits expecting them
  to persist — `tput setaf 1` is the honest example, and its colour is
  now cleared. Accepted by user decision (2026-09-12) as the cost of
  the fix.

## Regression history

- **0.7.109** — user reported dim text twice. The first investigation
  fixed a genuine truncated-SGR defect (ANTS-5130) and then concluded
  the residue was Claude Code's own `#999999`, on a capture that had
  found no `ESC[2m`. That capture was a short TUI run which never
  printed a dim line, and the conclusion was wrong. The user's second
  report — dim text in the terminal with no session running — is what
  reopened it. ANTS-5135.
