# Feature: `shellQuote` whitelist over denylist

## Problem

The 2026-04-27 `/indie-review` (ANTS-1047) flagged that
`src/shellutils.h::shellQuote` used a *denylist* regex
`[\s'"\\$`!#&|;(){}]` to decide when to wrap a value in single quotes.
The denylist missed:

- glob wildcards: `*`, `?`
- redirection / process-substitution markers: `<`, `>`
- bracket-expansion: `[`, `]`

A path containing any of those characters was returned **unquoted**,
so the shell saw it as a glob pattern or redirection target. Concrete
attack: a `cd` payload built from `shellQuote(workDir)` where
`workDir = /tmp/foo*` would expand to whatever `/tmp/foo*` matched
on the calling side.

## External anchors

- [POSIX shell quoting rules](https://pubs.opengroup.org/onlinepubs/9699919799/utilities/V3_chap02.html#tag_18_02_03)
  — the canonical safe-character set for unquoted tokens.
- [bash(1) §QUOTING](https://www.gnu.org/software/bash/manual/html_node/Quoting.html)
  — single-quote semantics: every character literal except the closing
  quote itself.

## Contract

### Invariant 1 — empty input maps to `''`

`shellQuote("")` returns the literal two-character string `''`. An
unquoted empty token would vanish from argv.

### Invariant 2 — safe-character whitelist passes through unchanged

`shellQuote(s)` returns `s` unchanged iff every character in `s` is in
the set `[A-Za-z0-9_\-./:@%+,]`. Concrete cases:

- `/home/user/project` → `/home/user/project`
- `foo-bar.txt` → `foo-bar.txt`
- `user@host.com:22` → `user@host.com:22`
- `value%20encoded+stuff,more` → `value%20encoded+stuff,more`

Note that `=` is NOT in the safe set — POSIX assignment-word semantics
mean a leading `name=` token can establish a variable assignment, so
`=` always forces single-quote wrapping (even though `=` mid-token is
harmless inside argv). The conservative whitelist matches the
indie-review specification.

### Invariant 3 — any character outside the whitelist forces quoting

The previously-missed glob/redirection/bracket characters get quoted:

- `/tmp/foo*` → `'/tmp/foo*'`
- `/tmp/?ar` → `'/tmp/?ar'`
- `/tmp/[a-z]` → `'/tmp/[a-z]'`
- `name<input.txt` → `'name<input.txt'`
- `name>output.txt` → `'name>output.txt'`

### Invariant 4 — embedded single quotes round-trip correctly

`shellQuote("it's")` returns the POSIX-canonical
`'it'\''s'` form (close-quote, escaped quote, reopen-quote). The
output, when fed to `/bin/sh -c 'echo X'` via interpolation, recovers
`it's` verbatim. Verified by literal comparison in the test, not by
spawning a shell.

## Architectural invariants

1. **Single source of truth.** All callers go through
   `shellQuote(QString)`. The test source-greps for any caller that
   open-codes single-quote escaping near a call to a shell launcher
   (e.g. `QProcess::start("sh -c ...")`) — none should exist.

2. **No regex-builder API leaks.** The internal whitelist regex stays
   `static const` inside the function body so the compiled regex is
   reused across calls (matches the existing pattern in
   `terminalwidget.cpp::pasteRiskReasons`).

## Test approach

Pure C++ unit harness — `shellutils.h` is header-only and Qt-only
dependent on `QString` + `QRegularExpression`, so the test links only
`Qt6::Core`. No widgets, no display.

The pre-fix denylist regex would have failed Invariants 3 and 4 cases
involving glob characters; Invariant 4 is a regression test for the
single-quote-round-trip shape.

## Source

`indie-review-2026-04-27` ANTS-1047, shipped 0.7.57.
