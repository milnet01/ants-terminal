# ADR-0002: Cold-eyes review of the Claude-Code companion bundle — review loop log

Record for [`../decisions/0002-cold-eyes-companion-cleanup.md`](../decisions/0002-cold-eyes-companion-cleanup.md) § Cold-eyes review pass 1 (2026-04-30).

## Cold-eyes loop log

This ADR was reviewed against itself before the user signed it.
Findings folded back in:

- D-1 Resolved: ANTS-1119 / ANTS-1120 IDs no longer presumed
  pre-allocated; allocation happens only after user sign-off.
- D-2 Resolved: ANTS-1114 trim explicitly deferred to user
  decision (was previously presented as if the ADR had picked).
- D-3 Resolved: memory-drift drop now has a one-paragraph
  reasoning citing the alternative coverage path.
- D-4 Resolved: "links invalidated" language clarified — IDs
  are stable; only bullet bodies change.
- D-5 Acknowledged: some Decision/Consequences overlap remains
  by design (decisions specify the *what*, consequences the
  *implications*). Not duplicated content; matched perspectives.
