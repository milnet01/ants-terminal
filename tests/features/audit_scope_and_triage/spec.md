# Feature: audit changed-lines scope and batch triage

## Invariants

**INV-1 — a run keeps the sets the Since baseline pill reads.** `runAudit`
computes the changed-line sets when `m_sinceBaseline` is on, not only when
the recent-files scope is.

**INV-2 — a failed git run is reported, not read as no changes.**
`readRecentChangeSets` returns the reason in `error`, which
`requestRecentChangeSets` stores in `m_recentScopeError`, when a git run
fails, times out or writes too much. A repository without `HEAD~N`
diffs against the empty tree (`git hash-object -t tree /dev/null`).

**INV-3 — the recent filters stand down on that error.** `handleCheckOutput`
applies the recent-files filter only while `m_recentScopeError` is empty.
Every Since baseline call site goes through `sinceBaselineVisible`, which
keeps only the baseline half while the error is set. `renderResults`
prefixes the status line with the reason.

**INV-4 — batch triage is bounded.** The dispatch loop appends to
`m_triageBatchQueue`; `pumpTriageBatches` sends while fewer than
`kMaxTriageInFlight` are in flight. `requestAiTriageBatch` creates no
`QNetworkAccessManager` of its own and does not re-render the list itself.

**INV-5 — triage replies are capped.** Both `requestAiTriage` and
`requestAiTriageBatch` call `capTriageReply(reply)`, which aborts a reply
past `LlmClient::kMaxBytes`.

## Rationale

The ANTS-5084 performance pass found each. With the pill on and the recent
scope off, a run cleared the sets and hid every filed finding. A short
repository failed `git diff HEAD~N` and showed zero findings. Every batch went
out at once on its own manager, each re-rendering the list, with no reply
size cap.

## Test surface

`test_audit_scope_and_triage.cpp` reads `src/auditdialog.cpp` (located from
the test's own path) and checks the named function bodies.

## Regression history

- **ANTS-5084:** the defects above. Locked by this spec.
