# ANTS-4471 — a `feedback_query` miss offers candidates, or says why it cannot

## Background

On a derived-path miss `feedback_query` returned `found:false` and a
reason, and nothing else. Its sibling `feedback_log op:"append_tracking"`
has answered a missing file with `candidates` + `hint` since ANTS-3366,
and `feedback_query` is usually the FIRST call a session makes — so it
is where the hint would pay most.

**The verb was never missing the feature.** `fbNotFound` already emits
`candidates` + `hint`; it calls `feedbackSiblingCandidates`, which
scanned exactly one directory — `QFileInfo(candidatePath).absoluteDir()`.
On the reported repro that directory is `/home/ants`, which holds no
feedback file, so `cands` came back empty and `fbNotFound` returned
before it could attach either field. The feature had nothing to find.

**The blocker, and why it needed a decision.** "Also scan the
conventional shared root" presumes a shared root the code can derive, and
there isn't one. For every other project the shared root IS the parent of
`caller_cwd`, already scanned. For `~/.claude` it is a different
filesystem branch entirely — two mismatches at once: the corpus is not
the caller's parent, and the leaf `.claude` is not the file's
`claude_config`. Nothing in the path connects them, so the root must be
told rather than derived. Hardcoding any absolute path would be wrong on
every other machine.

Three routes were filed rather than guessed at. This implements route 1,
the one the item recommended: a config key, defaulting to the existing
behaviour. Route 2 (read the roadmap store's `project` table) was
rejected because it buys a dependency for a hint; route 3 (scan each
tab's parent) fails for exactly the single-tab case where the hint is
most wanted.

## Invariants

### INV-1 — a miss with no key set is actionable, not terminal

When no candidate is found, the envelope carries `searched` (the
directories actually looked in) and a `hint` naming
`claude.mcp_feedback_root`. `found:false` alone is equally consistent
with "no file filed here yet", "you are one character out in the
basename", and "the corpus is not where I looked" — and on the reported
repro it was the third.

The miss rides ANTS-4104's envelope: a DERIVED-path miss is `ok:true`
with `found:false`, not a refusal. An EXPLICIT path that does not resolve
stays `ok:false`.

### INV-2 — a configured root is searched

With `claude.mcp_feedback_root` set, `*_Ants_MCP_Feedback.md` files in
that directory appear in `candidates`, even though no path rule could
reach it from `caller_cwd`.

### INV-3 — the configured root ADDS, it does not replace

The derived directory is still scanned when the key is set, so
configuring the key cannot break the common case of a project sitting
beside its corpus. Results are deduped by absolute path, so a key
pointing at the directory that would have been scanned anyway yields one
entry per file, not two. Order is derived-directory-first, so ANTS-3376's
own-file-floats-first ranking still applies.

### INV-4 — empty means the old behaviour

The key defaults to empty, and empty means "the parent of `caller_cwd`" —
the rule the scan already used. A project that never sets it behaves
exactly as before.

## ANTS-4896 — the same key, one call earlier

`feedback_query` consults `claude.mcp_feedback_root`; `session_orient`'s
`feedback_pending` block hardcoded the parent of the project root. One
build answered the same question two ways, and the block that exists to
show the maintainer their backlog was the one that could not see it.

Measured 2026-09-06: the corpus moved to a folder of its own and
`feedback_pending` reported `files_scanned:0` — byte-identical to
"nothing is waiting".

The scan moved into `RemoteControl::buildFeedbackPendingBlock` in the
same change. `cmdSessionOrient` refuses `no_window` without a MainWindow,
so the block could not be driven from a test at all; what stood in for it
was a source scrape of the bundle's body.

### INV-5 — the configured root is scanned

With `claude.mcp_feedback_root` set, feedback files in that directory are
read, listed and counted, and `shared_root` reports the declared corpus
rather than a parent directory holding nothing.

### INV-6 — an empty scan says where it looked

The block carries `searched` — every directory actually read.
`files_scanned:0` alone is equally consistent with "no contributor input"
and "the corpus is not where I looked", and those two want opposite
responses from the reader.

### INV-7 — the configured root ADDS, it does not replace

Both roots are scanned, matching INV-3 on the query side, so setting the
key cannot cost a project sitting beside its corpus. Files are deduped by
canonical path, so one directory reachable by two spellings is counted
once.
