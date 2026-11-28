# audit_dialog_review_fixes — the smaller audit-dialog bugs from the 2026-11-06 reviews

**Bundle:** `test_dialogs` · **Suite:** `AuditDialogReviewFixes`

ANTS-6621. Each invariant names its row in
`docs/reviews/close-findings-2026-11-06.md`.

## Invariants

- **INV-1 — a filtered run writes no trend snapshot** (row 292). `finishRun`
  saves a snapshot only when the run was not cancelled and none of New-only,
  Since baseline or Recent-only is on.
- **INV-2 — blame reads the working tree** (row 294). The blame command names
  no revision, and `runAudit` clears the blame cache.
- **INV-3 — a debt prompt drops its finding by content** (row 295). After the
  modal prompt, the allow and defer paths remove the finding they prompted
  for, matched by its fields, never by the index taken before the prompt.
- **INV-4 — the Claude hand-off is scrubbed** (row 298). The Review-with-Claude
  handler writes `SecretRedact::scrub(claudeReviewHandoff())`.
- **INV-5 — an AI rationale justifies only a false positive** (row 299). The
  SARIF suppression falls back to `aiReasoning` only when the verdict is
  empty or `FALSE_POSITIVE`.
- **INV-6 — the SARIF fingerprint holds no line number** (row 300).
  `primaryLocationLineHash/v1` is `computeFingerprint(file, checkId, message)`.
- **INV-7 — project text is escaped in rich text** (row 302). The project
  path label and the trend line's timestamp go through `toHtmlEscaped()`.
- **INV-8 — a stuck process is replaced** (row 303). Before a check starts,
  `runNextCheck` hands a still-running process to `releaseProcess` and runs
  the check on a new one.
- **INV-9 — a `%N` in a name is not filled in** (row 305). In the hand-off
  text, a file named `src/a%1.cpp` and a check named `rule %6` appear as
  written. The results pane's file:line takes one multi-arg `arg`.
- **INV-10 — a batch verdict lands only on a key the batch sent** (row 306).
- **INV-11 — batch triage with AI off says so** (row 1322). The AI-off return
  in `requestAiTriageBatch` sets the status line first.
- **INV-12 — a filtered view shows no trend line** (ANTS-6982).
  `trendLineHtml` returns nothing while New-only, Since baseline or
  Recent-only is on, as INV-1 saves no snapshot then.
- **INV-13 — `todo_scan` reads comments** (ANTS-6982). Its registration sets
  `codeLinesOnly` false.
- **INV-14 — Defer all files what it prompted for** (ANTS-6982, INV-3's race).
  After the heading prompt, the handler defers the list it took before the
  prompts and drops each of those findings by content; it never defers or
  clears `m_debtFindings` itself.
- **INV-15 — an earlier run's blame is dropped** (ANTS-6982). `runAudit` bumps
  `m_blameGeneration` with the cache clear, and a blame job caches its result
  only when the generation it started under is current.

## Test surface

INV-9's hand-off half builds the text with `claudeReviewHandoffForTest`. The rest
read the AuditDialog sources with comments stripped.

## Regression history

- **ANTS-6621:** INV-1 to INV-11. Locked by this spec.
- **ANTS-6982:** INV-12 to INV-15, found by the ANTS-6621 helpers.
