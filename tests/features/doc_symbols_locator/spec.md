# Feature: `doc_symbols` locator mode (ANTS-5313)

## Problem

A reviewer's expensive half is locating the code a document names.
`doc_symbols` already resolves every name, but its default reply lists every
occurrence and every definition, which is large. A lane wants one `file:line`
per name, and to be told plainly where one answer would be a guess.

Origin: `~/.claude/docs/reviews/v2-mechanical-checks-proposal-2026-09-24.md`
§ The one thing worth building.

## Contract

`doc_symbols {path, mode:"locator"}` returns
`{ok, mode:"locator", locators:{<symbol>:"<file>:<line>"},
ambiguous:{<symbol>:n}, unresolved:[<symbol>], not_checked:[<symbol>],
declared_only:[<symbol>], counts, truncated, checked_docs, docs_digest}`.

The reduction is `DocSymbols::locate()` (engine, pure); the JSON is
`RemoteControl::docSymbolsBuildLocatorResponse()` (pure).

- **INV-1 one entry per distinct symbol** — a symbol written N times appears
  once, in exactly one of the five buckets. `counts.symbols` is the distinct
  count and equals the sum of the five bucket counts.
- **INV-2 a definition beats a declaration** — where a symbol has one
  `definition` match and any number of `declaration` matches, its locator is
  the definition.
- **INV-3 more than one candidate is ambiguous, never a guess** — two or more
  distinct definitions put the symbol in `ambiguous` with that count. With no
  definition, the same rule applies to declarations: one is a locator, two or
  more are ambiguous.
- **INV-4 duplicate matches collapse** — two matches at the same `file:line`
  are one candidate.
- **INV-5 not_checked is never unresolved** — a needle the run never looked
  up lands in `not_checked`, not `unresolved` (ANTS-3661 INV-7).
- **INV-6 the locator reply carries no occurrence rows** — no `symbols[]` and
  no `findings[]`; that is the whole saving.
- **INV-7 refusals** — an unknown `mode` refuses `bad_args`; `mode:"locator"`
  with an `only` other than `all` refuses `bad_args`, since `only` filters rows
  this mode does not emit. The schema lists `mode` with both values.

- **INV-8 a local or a forward declaration is never a candidate** — a
  `local` row (declared inside a function body or a parameter list, the
  resolver kind ANTS-5313 added) and a `class X;`-shaped forward declaration
  are not places a reader wants. They neither locate a symbol nor make it
  ambiguous. A symbol with only such rows goes to `declared_only`: it is
  declared, so `unresolved` would be false, but there is no place to point.

## Reload

Compiled change: reaches a running terminal only after a relaunch.
