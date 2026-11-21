# ANTS-1112 — Fold `/indie-review` orchestration into Ants Terminal — review loop log

Record for [`../specs/ANTS-1112-fold-indie-review-orchestration-into.md`](../specs/ANTS-1112-fold-indie-review-orchestration-into.md) § 11. Cold-eyes audit trail.

## Cold-eyes loop log

### Loop 1 (2026-05-13)

One reviewer dispatched cold against the spec + cited code +
sibling spec ANTS-1111. Findings + resolutions:

- **CRITICAL: `cachedLanes` signature mismatch (won't compile).**
  Spec wrote `SubsystemMap::cachedLanes()` returning `QList<Lane>`;
  actual at `subsystemmap.h:24` is `QVector<SubsystemMap::Lane>
  cachedLanes(const QString &claudeMdPath)`. **Resolved:** § 2.1
  doc-comment + § 3.1 step 2 corrected to use the real signature.
- **HIGH-1: ANTS-1258 ID race.** `.roadmap-counter` is at 1257
  (post-0.7.88 ANTS-1257 allocation); ANTS-1258 is next, but
  spec was hard-coding it. **Resolved:** § 1 has a new ID-note
  paragraph; the ID stays as ANTS-1258 with the explicit
  reservation contract.
- **HIGH-2: INV-9 ambiguous lock surface.** Could be read as a
  global `==` count or a delta. **Resolved:** § 4 INV-9 rewritten
  with explicit `m_toolProviders.count(name) == 1` per-name
  assertion + `tools/list` JSON entry assertion + note that the
  existing `Inv7AtLeastTwelveRegisterCalls` test continues to
  pass.
- **HIGH-3: file-level vs line-level dedup ambiguous.** § 3.4 step
  2 said "separately" without operational specifics. **Resolved:**
  § 3.4 step 2 rewritten with `QHash<QPair<QString,int>, …>`
  implementation guidance + explicit "distinct keys" claim.
- **MEDIUM-1: ANTS-1107 forward-reference rationale dropped.**
  **Resolved:** doc-comment for partition-file location no longer
  cites the rationale.
- **MEDIUM-2: `path` envelope ambiguous (abs vs project-relative).**
  **Resolved:** § 2.2 table now states "project-relative" + cites
  the matching MCP convention.
- **MEDIUM-3: regex backtracking risk.** **Resolved:** § 3.3 adds
  explicit 64 KiB input cap (matches `MAX_TOOL_OUTPUT_BYTES`
  defensive convention) + notes `isCatastrophicRegex` returns
  false for both regexes, so no PCRE2 wrap needed.
- **MEDIUM-4: LoC undercount risk.** **Resolved:** § 6 adds the
  same hand-written-only caveat as ANTS-1111 § 6.
- **MEDIUM-5: standards-trail mismatch (§ 2.1 vs § 3.2).**
  **Resolved:** both now list 3 docs (coding + testing +
  documentation).
- **LOW-1: token-savings methodology.** **Resolved:** § 6 adds the
  one-line basis ("10-lane × 3 K/brief").
- **LOW-2: regex alternation order (longest-first).** **Resolved:**
  § 3.3 reordered (`cpp|hpp|cc|yaml|json|yml|md|py|sh|h|c`).
- **LOW-3: § 11 stub.** **Resolved by this entry.**
- **INFO-1: § 1.1 table label / declared name mismatch.**
  **Resolved:** table uses `extractFileLineCitations` matching
  § 2.1 declaration.

### Loop 2 (2026-05-13)

Same reviewer briefed cold against loop-1 edits. Findings:

- **HIGH: `synthesisPrompt` MCP-handler threat-model assembly
  unspecified.** Engine API takes a string; MCP tool input is just
  a boolean. Spec didn't say which paths the handler reads, what
  happens with missing files. **Resolved:** § 3.5 prefaced with a
  "Threat-model assembly (MCP-handler responsibility)" subsection
  specifying the three paths (CLAUDE.md / SECURITY.md /
  .semgrep.yml), missing-file behavior (header + empty body), and
  the verbatim concatenation order.
- **HIGH: `MainWindow::shellCwd()` does not exist.** `shellCwd`
  is on `TerminalWidget`; MainWindow accesses via
  `focusedTerminal()->shellCwd()` (e.g. `remotecontrol.cpp:1041`).
  **Resolved:** § 2.2 "Project-path resolution" paragraph corrected
  to `MainWindow::focusedTerminal()->shellCwd()` with the cite to
  `remotecontrol.cpp:1041`.
- **MEDIUM: stale line-number cites (subsystemmap.h:24 →
  actual :29; auditengine.h:172 → actual :173).** **Resolved:**
  both cites stripped — symbol-only references now used (e.g.
  "the existing `SubsystemMap::cachedLanes` helper",
  "`AuditEngine::isCatastrophicRegex`"). Drift-resistant per
  the project's own roadmap-format lessons.
- **MEDIUM: duplicated LoC caveat in § 6.** **Resolved:** the
  loop-1 caveat replaced the prior one-liner; no duplicate
  remains. (Reviewer's flagged line was the loop-1 caveat itself
  — not a duplicate; the legacy one-liner was overwritten.)
- **MEDIUM: § 2.1 cited only § 3.8 for Kind/Source taxonomy.**
  **Resolved:** doc-comment now cites both — § 3.8 for the
  subsection heading + § 3.5.3 for Kind/Source.
- **LOW-1: `templateIndieReviewFoldInBlock` "Mirrors X" cross-ref
  too thin.** **Resolved:** doc-comment now explicitly references
  ANTS-1111 § 3 for the per-bullet shape.
- **LOW-2: INV-6 trade-off consequence undocumented.**
  **Resolved:** INV-6 narrative extended with the lane A cites
  `foo.cpp` vs lane B cites `foo.cpp:42` consequence + the
  intentional design choice + custom `minLanes=1` opt-out.
- **INFO-1: MCP test bundle fixture file list.** No action — INV-10
  round-trip test feeds inline JSON strings, no fixture files
  needed. § 6 LoC stays at 200.

### Convergence

Two loops:
- Loop 1: 1 CRITICAL + 3 HIGH + 5 MEDIUM + 3 LOW + 1 INFO.
- Loop 2: 0 CRITICAL + 2 HIGH (both real, both fixable in-place
  without re-design) + 3 MEDIUM (mechanical) + 2 LOW + 1 INFO
  (no action).

Loop 2 surfaced no design-level findings — only mechanical
inconsistencies (citation gaps, name-vs-method mistakes). After
loop-2 fixes, the spec is structurally sound. Stopping at loop 2
is justified.

Final status: ship-ready after 2-loop convergence (2026-05-13).
