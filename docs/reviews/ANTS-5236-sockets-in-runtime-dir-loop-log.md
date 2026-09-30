# ANTS-5236 — cold-eyes loop log

Review history for [`docs/specs/ANTS-5236-sockets-in-runtime-dir.md`](../specs/ANTS-5236-sockets-in-runtime-dir.md).

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|---|---|---|---|---|---|---|---|
| 1 | 2026-10-01 | 2 (neutral-lane, every lane held every question) | 3 | 1 | 0 | 2 | Verified 6, fixed 6, dismissed 1. Both lanes: INV-5's legacy hook case needed the real /tmp against § 6's scratch rule (legacyDir parameter added); INV-1 said 'not under tempPath when XDG_RUNTIME_DIR is set', false for a set-but-0755 dir (reworded, test layout pinned). Lane 1: the bridge test would find real sockets (runtime pattern made a module variable, every bridge case isolated). From open questions: ANTS-4932 picker is § 2.1 not § 2.2; ANTS-5144 INV-8 'unchanged' added to § 7; Qt fallback dir's guessable name stated. Bridge wording now covers a set-but-unusable XDG_RUNTIME_DIR. Dismissed: two terminals with different runtime dirs rewriting the forwarder (local detail). Lane spend: about 147k input, 9.4k output, 2 turns each. |
