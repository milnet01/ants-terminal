# spec_lint — verb-layer conformance

Contract for `tests/features/spec_lint_verb/test_spec_lint_verb.cpp`. Owning
spec: [`docs/specs/ANTS-3662.md`](../../../docs/specs/ANTS-3662.md).

The handler needs a live `MainWindow`, so this lane splits the way ANTS-3601 and
ANTS-3661's verb lanes do: behavioural rows drive the pure helpers
(`SpecLint::check`, `RemoteControl::specLintBuildResponse`), and wiring rows
source-scrape the registration sites.

## What each row locks

| Row | Invariant | Claim |
|---|---|---|
| `Inv4CommandClauseIsACandidate` | INV-4 | A command clause with no stated expectation fires as a **candidate** — never `autoFixable`, `auto_fixable` absent from the wire — and the engine executes nothing. |
| `Inv7RefusalMinimums` | INV-7 | `caller_cwd` Required at both declaration sites; `path` validated before enumeration; nothing to scan → `ok:true` with empty findings. |

## Why two halves of INV-4 are a scrape

"The verb runs no subprocess" is a claim about code that **does not exist**, and
a behavioural test cannot hold an absence: a run that spawns nothing and a run
that spawns something the test never triggers are indistinguishable from the
outside. The scrape asserts `speclint.cpp` contains no `QProcess`,
`std::system`, `popen` or `execve`.

The `auto_fixable` half is a scrape of a different kind — of the **wire form**,
not the source. ANTS-3664 INV-1 omits the key when the flag is false, so the
assertion is that the key is *absent*; a test written against
`auto_fixable:false` passes against a non-conforming serialiser and fails
against a conforming one.

## The five-clause fixture

`Inv4CommandClauseIsACandidate` feeds five clauses and expects exactly two to
fire. The three that do not are the point, and each is excluded by a **different
half** of the heuristic:

| Clause | Fires | Excluded by |
|---|---|---|
| `` `grep -c foo src/` `` | yes | — a bare command states nothing it should return. |
| `` `grep -c foo src/` → 3 `` | no | The trailing-alphanumeric rule. |
| `` `tests/features/spec_lint/` covers it `` | no | The trailing-alphanumeric rule **again** — the trailing prose excludes it whatever the vocabulary says, so this clause does **not** test the vocabulary. |
| ``run `ctest -R spec_lint`.`` | yes | — trailing punctuation is not an expectation. |
| `` `tests/features/spec_lint/` `` | no | The fixed vocabulary, and only that. A bare path span with nothing after it. |

## Verified RED before the implementation landed

| # | Mutation | Result |
|---|---|---|
| M1 | Set `autoFixable = true` on the candidate finding | RED — the `EXPECT_FALSE(f.autoFixable)` row fails. |
| M2 | Widen the command test from the vocabulary to "the span contains `/`" | RED — the bare-path clause fires, giving 3 candidates where 2 are expected. **Survived the first run**, when the fixture's only path clause had trailing prose and was therefore excluded by the *expectation* rule rather than by the vocabulary — leaving the vocabulary's tightness untested. The bare-path clause was added for exactly this. |
| M3 | Treat any trailing text as an expectation (drop the alphanumeric rule) | RED — the `ctest` clause stops firing, giving 1 where 2 is expected. |
| M4 | Emit `sections_checked` only when true | RED — `Inv7`'s `contains("sections_checked")` row fails, which is the whole reason the field is never omitted. |
