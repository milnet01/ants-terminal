# Feature: every source of a discovered project path is traversal-checked

## Problem

`ClaudeIntegration::discoverProjects` resolves each project's real
filesystem path from three sources, in this order:

1. session metadata under `~/.claude/*` — a `cwd` field in a JSON file;
2. the transcript's own `cwd` field, via `extractCwdFromTranscript`;
3. `decodeProjectPath`, which reverses Claude Code's `/`-to-`-`
   directory-name encoding by probing the filesystem.

Only the second is checked. ANTS-1670 M3 added `isSafeAbsolutePath` —
absolute, no NUL, no `..` component — and applied it to the transcript
`cwd`, with the reasoning that a transcript is attacker-influenceable
and its `cwd` "flows on to launch/resume a `claude` process working
directory and a project root".

That reasoning applies unchanged to the other two, and the ordering
makes it worse: the *ungated* session-metadata value is tried FIRST, so
when both are present the checked one never runs. The gated source is
the one that loses.

`decodeProjectPath` can also emit a `..` component directly. It splits
the directory name on `-` and joins with `/`, so a directory named
`-..-..-etc` decodes to a path that climbs out of the tree. Its own
comment argues the risk is low — the names are written by Claude Code
under the user's own UID, and the probes are existence checks with no
write and no symlink follow — and that argument is sound for the
*probing*. It is not an argument about the value it returns, which is
what the caller then uses as a project root.

## Contract

A project path MUST satisfy `isSafeAbsolutePath` before
`discoverProjects` publishes it, whichever source produced it.

Two changes express this:

1. Session-metadata `cwd` is checked where it is stored, so the lookup
   map holds only safe values and an unsafe one falls through to the
   next source rather than shadowing it.
2. `decodeProjectPath` returns an empty string rather than a path
   containing a `..` component. Empty is the honest answer — there is
   no safe path to recover from that name — and the caller already has
   an empty-means-unresolved branch.

A single check on the resolved value then covers all three, and a
project whose path cannot be trusted is skipped rather than published
with a path nobody validated.

## Invariants

**INV-1 — `decodeProjectPath` rejects a traversal.** Given a name that
decodes to a path with a `..` component, it returns an empty string.

**INV-2 — `decodeProjectPath` still decodes an ordinary name.** A name
with no traversal decodes as before, so the gate does not cost the
normal case.

**INV-3 — `decodeProjectPath` rejects a name that is not
absolute-encoded.** A name not starting with `-` cannot be an encoded
absolute path; it returns empty rather than a relative path the caller
would use as a project root.

**INV-4 — session-metadata `cwd` is gated at the point it is stored.**
Source-grep `discoverProjects`: the assignment into the session-cwd map
tests `isSafeAbsolutePath`.

**INV-5 — the resolved path is gated before publication.** Source-grep
`discoverProjects`: after the three-source resolution, an
`isSafeAbsolutePath` check guards the project being appended.

## Scope

### In scope
- Runtime test of `decodeProjectPath`, which is a public static and
  needs no instance.
- Source-grep for the two `discoverProjects` gates, which need a
  populated `~/.claude` tree and a `ClaudeIntegration` instance.

### Out of scope
- `extractCwdFromTranscript`, already gated by ANTS-1670 M3 and
  unchanged here.
- The existence probing inside `decodeProjectPath`. ANTS-1845 assessed
  it and its assessment stands; this is about the returned value.
- Whether a project path should also be required to EXIST.
  Deliberately not required, matching ANTS-1670 M3's reasoning: a
  validly recorded project may have moved, and a stale path fails a
  later launch harmlessly where a traversal does not.

## Regression history

- **ANTS-1670 M3:** added `isSafeAbsolutePath` and applied it to the
  transcript `cwd`. The session-metadata twin and the decode fallback
  were not covered.
- **ANTS-4457 (cold sweep 2026-08-18, verified 2026-09-08):** reported
  as "session-metadata `cwd` bypasses `isSafeAbsolutePath` while the
  transcript-derived twin is gated; session metadata is tried FIRST,
  and `decodeProjectPath` is ungated too and can emit `..`". Verified
  against source in all three parts and fixed. Locked by this spec.
