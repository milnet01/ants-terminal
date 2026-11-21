# ANTS-1877 — `roadmap_log op:append` stable-prefix project hint — review loop log

Record for [`../specs/ANTS-1877-roadmap-log-op-append-stable.md`](../specs/ANTS-1877-roadmap-log-op-append-stable.md) § 8. Cold-eyes loop log.

## Cold-eyes loop log

- **Loop 1 (2026-05-26).** Spec drafted with the diagnostic
  splits + sniffer contract. Verified against
  `src/remotecontrol.cpp` `cmdRoadmapLogAppend` (line 3102 — the
  block whose refusal needs splitting). Scope deliberately limited
  to Vestige's option (a); allocator support deferred (option b).
- **Loop 2 (2026-05-26).** Reviewer flagged 2 HIGH (§4 Tests
  missing INV-mapping / label / fail-first sentence per
  `specs.md:131-138`; decision table didn't address zero-byte
  `.roadmap-counter` case — recipe `echo 0 >` works but
  `touch` doesn't), 3 MEDIUM (mcp-error-codes category unstated;
  sniffer regex ≥2 char minimum undocumented; `follow_up`
  parity between the two new codes), 4 LOW/INFO polish. All
  verified against `cmdRoadmapLogAppend:3109-3116` (empty-file
  parse) + `docs/standards/mcp-error-codes.md:47-104`. Fixes:
  expand §4 with INV-mapping table + label + fail-first
  convention; add zero-byte-counter row to §2.1 table routing
  to `counter_missing`; name mcp-error-codes category § 2
  *Resource state*.
