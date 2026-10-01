# ANTS-5485 — loop log

Spec: [`docs/specs/ANTS-5485-retire-deprecated-verbs.md`](../specs/ANTS-5485-retire-deprecated-verbs.md)

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|---|---|---|---|---|---|---|---|
| 1 | 2026-10-01 | 2 (neutral-lane; every lane held every question) | 0 | 1 | 0 | 1 | Verified 2, fixed 2, dismissed 0. Q2 (both lanes): INV-6 forbade the `test_audit_synthesis_prompt` string that § 2.3's kept `TestAuditEngine::synthesize` still uses; § 2.1 now renames it and INV-6 names the two `current_state` lines it allows. Q4 (lane 1): INV-8's qualified names missed the unqualified definitions and hit a kept comment; replaced by five commands that each print lines today. Two open questions resolved clean (INV-10's `current_state` hit is a verb mention; CMake names the file in lowercase, now covered). |
| 2 | 2026-10-01 | 2 (neutral-lane; every lane held every question) | 0 | 1 | 0 | 0 | Verified 1, fixed 1, dismissed 0. Q2 (lane 1; lane 2 saw it and judged it a builder's choice): § 2.3 kept `detectorsByCategory` against its own delete rule; its only test caller is in a test this spec deletes, so it moves to the delete list and INV-8 checks the engine files. Three open questions resolved without a build change (§ 2.4's rollout wording for ants-mcpd clients, INV-3's thin test, the unquoted error text in `synthesize`); recorded, not fixed. Cap reached (2 for a spec): calm cap, 0 of 1 final-loop findings on text this run wrote. No second share: the spec was new, so the whole document was the armed change. Accepted; implementation is the next reviewer. |
