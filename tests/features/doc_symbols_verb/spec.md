# Feature: `doc_symbols` verb layer — vocabulary and refusal minimums (ANTS-3661)

## Problem

The engine's honesty guarantee (report, never judge) is a claim about the words
it emits, and no behavioural assertion can hold it: a verb that added a
`severity` field would still pass every engine row. The verb contract —
`caller_cwd` Required, `bad_path` on a root escape, `ok:true` on a path with
nothing to scan — is likewise wiring, not behaviour, and the handler needs a
live `MainWindow` no unit test has.

## Contract

Behavioural rows drive the pure helper `RemoteControl::docSymbolsBuildResponse`;
wiring rows source-scrape the registration sites (the `rc_get_text_byte_cap`
pattern, as `tests/features/doc_integrity_verb/` does).

- **INV-4 no defect vocabulary** — `src/docsymbols.cpp` and the body of
  `docSymbolsBuildResponse` carry no `severity`, `broken`, `invalid` or
  `autoFixable = true`. `src/docsymbols.h` is deliberately **not** scraped: it
  states the rule in prose, and a scrape that forbids naming the rule forbids
  documenting it.
- **INV-6 refusal minimums** — three arms. (1) `doc_symbols` is registered with
  `CallerCwdContract::Required` at the call site *and* in the static table
  `registerToolProvider` asserts against, and appears in the `tools/list`
  schema. (2) `cmdDocSymbols` validates a supplied `path` before any
  enumeration, so a root escape refuses `bad_path` first. (3) Nothing to scan
  is `ok:true` with an empty `symbols[]` — not a refusal (ANTS-3601 INV-15's
  shape).
- **Tri-state on the wire** — `resolved` / `unresolved` / `not_checked` reach
  the response as three distinct strings, with matching `counts`. The response
  is the last place a caller could still be told "does not exist" about a
  needle nobody looked up.
- **INV-8 provider install ordering (ANTS-3688)** — `mainwindow.cpp` installs
  the verb-vocabulary provider exactly once, after
  `m_remoteControl = new RemoteControl(` and before the definition of
  `setupClaudeMcpProviders()` — i.e. from the constructor, not from the
  registration function. Both offset assertions are needed: that function's
  *definition* sits later in the file than its *call*, so "after the
  allocation" alone is satisfied by the broken position.

### Verified RED by mutation

Both rows here assert an **absence**, so both pass against a verb that emits
nothing at all — the state they ran against before the implementation landed.
A row asserting an absence that no mutation can redden is not a test, so each
got one:

| Mutation | Row | Result |
|---|---|---|
| M6 emit `severity` vocabulary on every finding | INV-4 | RED |
| M7 return a canned non-empty `symbols[]` regardless of input | INV-6 arm 3 | RED |
| M9 move the provider install back into `setupClaudeMcpProviders()` | INV-8 | RED |

**M9 was not a hypothetical — it was the shipped state.** INV-8 was written
against `git show HEAD:src/mainwindow.cpp` before the fix landed, and the byte
offsets were compared directly: pre-fix `install=253253`, `setupDef=189762`, so
`install < setupDef` was **false** and the row is red; post-fix `install=54981`
against `alloc=54101` and `setupDef=190771`, and it is green. The
`install > alloc` assertion held in *both* states, which is precisely why it
cannot be the only one.

INV-1/2/3/5/7's mutations are recorded in `tests/features/doc_symbols/spec.md`.

## Out of scope

- **Driving `cmdDocSymbols` end-to-end** — it resolves the project root through
  a live `MainWindow`. Covered by the source-scrape split above; the E2E
  harness (`tools/e2e/`) is where a full-stack call would belong. A permanent
  exclusion for a unit test, not deferred work.
