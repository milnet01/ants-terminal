# Flatpak host tools — feature spec (ANTS-5527)

## Contract

Inside a Flatpak, ants-mcpd runs `rg`, `git`, `ctest`, `bash` and a
`mutation_probe` test command on the **host**, through
`flatpak-spawn --host`. The KDE runtime carries none of them. Outside a
Flatpak nothing changes. `src/hostexec.h` holds the one rewrite every
such call site uses.

## Invariants

**INV-1 — Outside a sandbox the launch is unchanged.**
`HostExec::wrap(program, args, dir, env, false)` returns `program` and
`args` exactly.

**INV-2 — Inside a sandbox the launch goes through `flatpak-spawn --host`.**
The program is `flatpak-spawn`. Its arguments start with `--host` and
`--watch-bus`, carry `--directory=<dir>` when `dir` is non-empty, then
`--`, then the original program and arguments in order. `--watch-bus`
stops the host process when ants-mcpd exits or is killed.

**INV-3 — A variable the caller set crosses as `--env=`.**
`flatpak-spawn --host` does not pass the caller's environment. Each
variable in `env` that is absent from, or differs from, `base` appears as
`--env=KEY=VALUE` before `--`. A variable equal to `base` does not. An
empty `env` (the caller set none) adds no `--env=` token.

**INV-4 — The MCP subprocess sites use the helper.**
None of the files listed in the test calls `QProcess::start` with a bare
`"rg"`, `"git"`, `"ctest"` or `"bash"` program name; each goes through
`HostExec::start`.

## Reload

ants-mcpd is a separate binary: a rebuild plus `/mcp` reaches a running
session. No terminal relaunch.
