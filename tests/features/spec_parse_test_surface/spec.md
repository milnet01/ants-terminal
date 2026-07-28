# spec_parse_test_surface — `test_surface` from both invariant forms (ANTS-3665)

`docs/standards/specs.md` § 6 promises that `spec_query` returns
`invariants:[{id, body, test_surface?}]`, and that "a `*Test:*` / trailing
`— test surface` clause is surfaced as `test_surface`".

Only the GFM table branch did that. The bullet form — which the *same* standard
(§ 3) calls the default, and which the overwhelming majority of this corpus
uses — emitted `{id, body}` and left the whole `*Test:* …` sentence buried in
`body`. Verified live before the fix: every `spec_query` call across the six
doc-lint specs returned invariants with no `test_surface` at all.

This is a parser bug, not a standard bug. The standard describes the behaviour
every caller already expects, and ANTS-3662 (`spec_lint`) is blocked on it — its
central check is "every `INV-N` carries a test surface", which cannot be asked
of a parser that never extracts one.

`SpecParse::parseSpecBody` is **pure**: spec text in, JSON out, no filesystem.
Every fixture here is a string literal.

## Contract

| INV | Claim | Test surface |
|---|---|---|
| INV-1 | A bullet-form invariant with a `*Test:*` clause yields `test_surface`, and the clause is **removed** from `body` | `Inv1BulletFormEmitsTestSurface` |
| INV-2 | An invariant with no clause **omits the key**, never emits an empty string | `Inv2NoClauseOmitsTheKey` |
| INV-3 | The GFM table branch is unchanged | `Inv3TableFormStillWorks` |
| INV-4 | The clause ends at its paragraph; trailing commentary stays in `body` | `Inv4ClauseEndsAtItsParagraph` |
| INV-5 | Several bullets in one section each keep their own clause | `Inv5MultipleBulletsEachKeepTheirOwnClause` |

## Why these assertions and not weaker ones

**INV-1 asserts `body` too, not just the new field.** A parser that *copies* the
clause into `test_surface` while leaving it in `body` passes a
presence-only check and produces two records of the same sentence. The table
form keeps them disjoint; the bullet form now does the same.

**INV-2 asserts absence, not emptiness.** `spec_lint`'s `invariant_no_test`
finding is exactly the question "is this key missing", so an empty string would
make "has no test surface" indistinguishable from "has a blank one".

**INV-4 exists because the obvious implementation is wrong.** Taking everything
after `*Test:*` swallows the commentary paragraphs that bullets in this corpus
routinely carry — prose about *why* the invariant is shaped as it is, which is
not test surface. The fixture therefore has a trailing paragraph where INV-1's
deliberately does not.

**INV-5 guards the bullet-boundary scan.** The multi-bullet path slices on the
next `- **INV-` anchor, and that is the part most likely to mis-slice once
bodies stop being a single line. Its third bullet is untested on purpose, so
the fixture covers "some have clauses, some do not" in one pass.

## The hoist

The same change moves `parseSpecBody` out of `src/remotecontrol.cpp`'s anonymous
namespace into `src/specparse.{h,cpp}` in `ants_core_lib`. ANTS-3662's engine
lives in that library and cannot link an anonymous-namespace function; without
the hoist it would have to grow a second spec parser beside this one — the
divergence `MarkdownScan` was hoisted (ANTS-3603) to prevent.

**That this test file links is the proof.** It includes `specparse.h` and calls
`SpecParse::parseSpecBody` from a `test_core` bundle target; before the hoist
there was no header to include.

## Verified RED first

Run against the hoisted-but-unfixed parser (2026-07-28): **INV-1, INV-4 and
INV-5 fail** on the `test_surface` assertions; **INV-2 and INV-3 pass**. That
split is the useful part — INV-3 passing confirms the table branch was never
involved, and INV-2 passing confirms the "no clause" case was already correct,
so the defect is localised to the bullet branch's extraction alone.

Recorded because the prediction was wrong in one place: three tests were
expected to fail, not two. INV-5 asserts clauses as well, which the first
reading of it missed.
