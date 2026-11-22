# ANTS-5010 — Warn when a keyless AI request goes out over plain http — review loop log

Record for [`../specs/ANTS-5010-plaintext-prompt-warning.md`](../specs/ANTS-5010-plaintext-prompt-warning.md) § Cold-eyes loop log.

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|---|---|---|---|---|---|---|---|
| 1 | 2026-09-10 | 3 | 0 | 2 | 0 | 0 | 2 findings, 2 verified / 0 dismissed, both fixed. Q2: § 2.2 said once per endpoint while naming one member for the last endpoint; now states the last-endpoint rule. Q2: § 2.3 set the review warning and nothing cleared it after an endpoint change; beginRound now clears it, INV-3 covers plain-http then https. Five open questions resolved clean, none a finding. Packet build found no defect. Loop 2 dispatched. |
| 2 | 2026-09-10 | 3 | 1 | 0 | 0 | 1 | 2 findings, 2 verified / 0 dismissed, both fixed. Q4 (lane 3): § 6 put every test endpoint on TEST-NET-1, which INV-2's loopback case cannot be; loopback now uses a port nothing listens on. Q1 (orchestrator, from a lane's open question): § 2.4 appended the triage warning to a label set to Qt::ElideRight, so it was cut first; it now comes first. Two lanes had no findings; five open questions resolved clean. Final-loop share on text this run wrote earlier: 0 of 2, a calm cap. Gated-span share: 4 of 4 run findings, the whole document being new. Cap reached; spec accepted, no deferred tail. |
| impl | 2026-11-22 | none | — | — | — | — | ANTS-5624 renamed the brief hook `briefWork`; two references updated, no behaviour change. Not a review loop; no reviewer was dispatched. |
