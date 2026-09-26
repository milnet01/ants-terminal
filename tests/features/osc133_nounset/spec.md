# The OSC 133 shell integration works in a shell running with `nounset`

`packaging/shell-integration/ants-osc133.{bash,zsh}` are sourced into the
user's interactive shell. Many users run `set -u` (bash) or
`setopt nounset` (zsh). A helper that reads an optional positional
argument as a bare `$2` then aborts on every one-argument call, so no
prompt marker is ever sent (audit TL-27).

## Invariants

- **INV-1** — with `nounset` on, a one-argument `__ants_osc133_emit A`
  exits 0 and writes an `OSC 133 ; A ; aid=… ; ahmac=…` sequence, in bash
  and in zsh.

## How it is tested

`test_osc133_nounset.sh` sources each file in a fresh non-interactive
shell with `nounset` on, `TERM_PROGRAM=ants-terminal` and a test key set
(the files return early without them), calls the emit helper with one
argument, and checks the exit status and the sequence. The zsh half is
skipped, with a message, when zsh is not installed, as on the CI runner.
It needs `openssl`, as the integration itself does.
