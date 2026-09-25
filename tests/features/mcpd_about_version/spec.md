# mcpd_about_version — ANTS-5340

The About dialog names the `ants-mcpd` build that Claude Code launches. It
reads that build when the dialog opens, so a rebuilt `ants-mcpd` shows with
no terminal relaunch. Code: `src/mcpdversion.h`.

## Invariants

- **INV-1** — `ants-mcpd --version` prints `mcpd::versionLine()` as its one
  stdout line and exits 0. It does not wait on stdin. The line starts with
  `ants-mcpd ` and the version, and names the build commit.
- **INV-2** — `mcpd::locateBinary()` returns the user-level
  `mcpServers.ants.command` from the Claude Code config when that file is
  executable. It wins over an `ants-mcpd` in the terminal's own directory.
- **INV-3** — With no usable config entry, `locateBinary()` returns the
  `ants-mcpd` in the terminal's own directory. "No usable entry" covers a
  missing file, a file that is not JSON, no `ants` entry, and a command that
  is not executable.
- **INV-4** — With neither, `locateBinary()` returns what a `PATH` search
  for `ants-mcpd` finds, which may be empty.
- **INV-5** — `mcpd::queryVersion()` returns the binary's `--version` line.
  It returns an empty string for a path that does not run.

## Cases

| Case | Invariant | How |
|---|---|---|
| `Inv1VersionFlagPrintsOneLine` | INV-1 | Run the built `ants-mcpd --version` with stdin left open; compare stdout with `versionLine()`. |
| `Inv2ConfigCommandWins` | INV-2 | A temp config naming an executable temp file, and a sibling `ants-mcpd` beside it. |
| `Inv3FallsBackToSibling` | INV-3 | The four unusable-config shapes, each with a sibling present. |
| `Inv4FallsBackToPath` | INV-4 | No config, an empty directory; compare with `QStandardPaths::findExecutable`. |
| `Inv5QueryVersion` | INV-5 | The built binary, then a path that does not exist. |
