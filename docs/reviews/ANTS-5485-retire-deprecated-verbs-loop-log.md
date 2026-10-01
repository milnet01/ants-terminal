# ANTS-5485 — loop log

Spec: [`docs/specs/ANTS-5485-retire-deprecated-verbs.md`](../specs/ANTS-5485-retire-deprecated-verbs.md)

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|---|---|---|---|---|---|---|---|
| 1 | 2026-10-01 | 2 (neutral-lane; every lane held every question) | 0 | 1 | 0 | 1 | Verified 2, fixed 2, dismissed 0. Q2 (both lanes): INV-6 forbade the `test_audit_synthesis_prompt` string that § 2.3's kept `TestAuditEngine::synthesize` still uses; § 2.1 now renames it and INV-6 names the two `current_state` lines it allows. Q4 (lane 1): INV-8's qualified names missed the unqualified definitions and hit a kept comment; replaced by five commands that each print lines today. Two open questions resolved clean (INV-10's `current_state` hit is a verb mention; CMake names the file in lowercase, now covered). |
