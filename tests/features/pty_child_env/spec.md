# Feature: the shell receives the terminal's whole environment

## Invariants

**INV-1 — no environment entry is dropped, however many there are.** A
variable set last in the terminal's environment, after enough other entries to
pass the fixed table `Pty::start` used to copy them into, reaches the child
shell with its value.

**INV-2 — the terminal's own overrides still apply.** The child sees
`TERM=xterm-256color` and `TERM_PROGRAM=AntsTerminal` in place of the parent's
values for those keys.

## Rationale

`Pty::start` built the child's environment in a fixed-size table and stopped
copying when it filled. Variables the terminal adds at run time, such as
`ANTS_MCP_SOCKET`, go at the end of the environment, so they were the first
lost on a large desktop session (ANTS-1897 INV-14). The table is now sized from
the environment before the fork.

## Test surface

`test_pty_child_env.cpp` pads the test process's environment past the old table
size, sets a sentinel last, starts a `Pty` on a script that prints the sentinel
and the two overrides, and reads what the child printed. Behavioural: only a
real child shows what its environment held.

## Regression history

- **ANTS-5075:** the child environment was cut off past a fixed entry count,
  dropping the variables added last. Locked by this spec.
