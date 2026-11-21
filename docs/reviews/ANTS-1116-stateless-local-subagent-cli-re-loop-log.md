# ANTS-1116 — Stateless local-subagent CLI (re-scoped v1, post cold-eyes) — review loop log

Record for [`../specs/ANTS-1116-stateless-local-subagent-cli-re.md`](../specs/ANTS-1116-stateless-local-subagent-cli-re.md) § Cold-eyes review pass 1 (2026-04-30).

## Cold-eyes loop log

Findings folded back in:

- 16-1 **Architectural fix:** `audit-run` removed from v1.
  Spec now describes a single-subcommand v1; `audit-run` is
  contingent on ANTS-1119 (audit-engine extraction).
- 16-2 Resolved by deferring `audit-run`. The shape question
  (custom Ants schema vs SARIF) gets re-litigated in the
  v2 spec.
- 16-3 Resolved: INV-2/3/4/5/6/8 specify exit-code semantics
  per subcommand outcome; INV-6 covers signal-kill case.
- 16-4 Resolved: INV-4 covers absent-script case.
- 16-5 Resolved: open question 1 rewritten — the flip trigger
  is concrete (ANTS-1120 keep verdict).
