# ADR-0004: Same-UID trust model for the MCP audit / test-audit / synth suite

- **Status:** Accepted
- **Date:** 2026-05-17
- **Deciders:** Project lead, Claude
- **Related:** ROADMAP.md ANTS-1351, ANTS-1397, ANTS-1352, ANTS-1416,
  ANTS-1404, ANTS-1372, ANTS-1448; CHANGELOG.md ANTS-1448 entry
  (filed under `[0.7.92] — 2026-05-20`).

## Context

ANTS-1351 (`audit_run` MCP verb), ANTS-1397 (`test_audit_*` MCP
trio), and ANTS-1352 (`indie_review_dispatch` orchestrator,
shipped in 0.7.91) each carry their own "out of scope: same-UID trust
model" caveats. Cold-eyes loop 3 on the ANTS-1397 spec
(2026-05-17) called out the recurrence — every spec re-derives
the trust boundary from scratch, so the boundary is restated four
or five times across the lane without anyone owning the canonical
statement.

This ADR pulls the trust model into one place. Future MCP audit /
review / synth specs cite it rather than re-deriving.

## Decision

**The MCP audit / test-audit / synth tool suite assumes a "same-UID
trust" boundary. Inputs and outputs that cross that boundary are
in scope for adversary modelling; everything inside it is trusted.**

### What the boundary covers

A process running under the Ants user's UID already has:

- Read access to every project file the user can read.
- Write access to every directory the user can write to.
- Visibility into every command the user runs (via `ps` /
  `/proc/<pid>/cmdline`).
- The ability to invoke `cat`, `grep`, `find`, `ripgrep`, `git`,
  `sed`, etc. against the project tree.

MCP verbs that leak file fragments, line numbers, stderr excerpts,
or directory listings add **nothing beyond what `cat` already gives
a same-UID process.** They are not "leaks" in the security sense;
they are convenience wrappers around access the attacker (if any)
already has.

### What the boundary does NOT cover

The trust assumption breaks the moment a tool's output is
**re-published outside the same-UID context**. Specifically:

- Forwarding an MCP envelope to a PR comment, public log file,
  shared Slack/Discord channel, screenshot, or chat transcript.
- Pasting an `audit_run` SARIF excerpt into a public issue tracker.
- Pushing a roadmap-log entry that quotes a private path or stderr
  line to a public repo.

In those cases the **assistant** (the agent reading the MCP
response) is responsible for redacting before re-publication. The
MCP server does NOT redact on egress because it can't reason about
the downstream consumer's audience.

### Concrete consequences for the audit / synth surface

| Concern | Specifically | Trust-model verdict |
|---------|--------------|---------------------|
| `audit_run` SARIF responses carry full file paths, line numbers, and source-snippet excerpts | Same-UID process can `cat` the source. | In scope under same-UID; assistant must redact before re-publication. |
| `test_audit_partition` includes test-file paths + symbol names | Same as above. | In scope. |
| Semgrep / cppcheck / clazy registry pulls may transmit project source patterns to the tool's update endpoint | Tool already does this when invoked from a shell. | Not Ants-specific; user opts in by installing the tool. |
| `pattern_id + file + line` triples in dispatch reports | Same-UID introspectible. | In scope. |
| TOCTOU windows between scan and apply on `reports_dir` | Same-UID can edit between scan and apply. | In scope; the apply step re-reads. |
| stderr leak from a spawned auditor | Same-UID can `strace` / `tee` the same stream. | In scope. |

### What the boundary STILL enforces

Same-UID trust is **not** an excuse for:

- **Cross-project leaks under the same UID.** A session in project
  A asking the MCP about project A must not get project B's data
  because Ants has a different tab focused. This is the
  confused-deputy attack ANTS-1404 (`CallerCwdContract::Required`)
  + ANTS-1372 (RcGate) close. The trust boundary is per-PROJECT,
  not per-UID — even though both projects live under the same UID.
- **PathValidation bypasses.** ANTS-1295 path anchoring is in
  effect; symlink escapes out of the caller's project root still
  refuse with `code:"bad_path"` regardless of UID. The trust
  boundary doesn't extend the project root.
- **Wrap-overhead manipulation.** ANTS-1294 envelope wrapping is
  still enforced; the trust model doesn't excuse a tool from
  declaring its content shape on the wire.

## Alternatives considered

1. **Tighter — sanitise every response.** Reject. The cost is
   ~every audit tool's primary value: pointing at the specific
   file/line so the user can fix it. Stripping that data ruins the
   tool's signal/cost ratio and forces the assistant to round-trip
   through `cat` anyway.

2. **Tighter — separate "low-trust" and "high-trust" tool tiers.**
   Reject. Adds a second classification axis on top of
   `CallerCwdContract`; no caller has asked for it; tier
   maintenance becomes its own drift surface (the ANTS-1417
   coverage test pattern would have to grow).

3. **Looser — trust the assistant to redact.** Reject as the
   *default*. The assistant is downstream of the MCP boundary;
   the MCP server doesn't know who will read its output. Same-UID
   trust is the right baseline; the redact-on-republication
   responsibility belongs to the assistant per use case (see
   ANTS-1294 wrap, which signals "this is data, not instructions"
   exactly to make redaction decisions tractable).

## Consequences

### Positive

- Future audit / review / synth specs (ANTS-1449, ANTS-1450, the
  ANTS-1352 dispatcher (shipped 0.7.91), cold-eyes engine work) cite
  this ADR instead of re-deriving the trust model. Spec authoring time
  drops; the boundary statement stays consistent across the lane.
- Indie-review and cold-eyes reviewers have a canonical reference
  to test individual spec claims against ("does this match
  ADR-0004?"), instead of doing the model-derivation work
  themselves.

### Negative

- The "what `cat` already gives" framing depends on the same-UID
  premise. If Ants ever ships a remote MCP transport (tcp, ssh,
  multi-user daemon), this ADR will need an explicit
  supersession or amendment — `cat` no longer applies once the
  caller process can be on a different UID.
- The ADR doesn't itself produce code; it's a documentation
  contract. Drift between code and ADR is possible (e.g. a future
  tool adds an actual cross-project leak path and someone forgets
  to flag it as outside the same-UID assumption).

### Mitigations

- A reference to this ADR has been pinned in `docs/standards/mcp-error-codes.md`
  § 3 (caller-cwd contract) — the closest neighbouring doc.
- Future ANTS-NNNN that ship a remote-transport MCP path MUST
  open a supersession ADR before merging.
