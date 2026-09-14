# Feature: every owner-only permission result is used

## Problem

`setOwnerOnlyPerms` (`src/secureio.h`) makes a file readable by its owner
only and returns whether that worked. Most call sites discarded the
result, so a file meant to be private could stay readable by other local
users with nothing to show for it.

## Contract

`secureio.h` states the policy (ANTS-5151, split by risk). A file holding
project content, terminal output, or a tool's or Claude's answers is not
written, or is removed, when the call fails. Every other file calls
`warnNotOwnerOnly` and carries on. Both overloads are `[[nodiscard]]`.

The compiler warns about a discarded result, but a warning does not stop
a build. This test is what makes a new discarded call a failure.

## Invariants

**INV-1 — no call discards the result.** No line in `src/` is a bare
statement consisting of a `setOwnerOnlyPerms(...)` call and its
semicolon. Every call is a condition or feeds one. Measured when this
test was written: 47 bare calls on the commit before ANTS-5151, 0 after.

**INV-2 — the policy lives in secureio.h.** Both overloads carry
`[[nodiscard]]`, and `warnNotOwnerOnly` is defined there.

## Scope

### Out of scope
- Which half of the policy each site takes. That is a judgement per
  file, recorded on ANTS-5151, and a scrape cannot check it.
- Proving the fail-closed branches at run time. `chmod` on a file the
  process just created does not fail on an ordinary filesystem.
