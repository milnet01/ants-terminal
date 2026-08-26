# mcp_result_offload — ANTS-2094 conformance

Test contract for proactive MCP result offload (observation masking).
Full design: `docs/specs/ANTS-2094.md`. Behavioural coverage of the
`mcp::` offload/spill API (in `ants_core_lib`) under an isolated test
cache dir (`QStandardPaths` test mode), plus source-scrapes for the
dispatch-site and verb-wiring invariants.

## Cases → invariants

| Test | Covers |
|---|---|
| `Inv1EligibilityAndRequestResolution` | INV-1 — eligible set is the large-body read verbs, separate from the compaction table; write/control-plane never eligible; `offloadRequested` resolves per-call over session default. |
| `Inv12ConfigClamps` | INV-12 — threshold clamps to `[4096,1048576]`, head to `[256,16384]`. |
| `Inv2And3EnvelopeAndSpillFile` | INV-2 (envelope: `offloaded`, 64-hex `handle`, `bytes`, `head` prefix, `head_truncated`, `hint` names `read_spill`) + INV-3 (spill file holds the body verbatim; `sha256(file)==handle`). |
| `Inv4OwnerOnlyPerms` | INV-4 — spill file is 0600 (group/other bits clear). |
| `Inv12HeadCharBoundary` | INV-12 — head cut never splits a UTF-8 multi-byte sequence. |
| `Inv5And6ReadSpillPaging` | INV-5/INV-6 — `readSpill` round-trips, byte-pages by returned `offset+bytes`, offset-past-end → empty/`!truncated`, unknown handle → `not_found`. |
| `Inv8IdempotentReSpill` | INV-8 — same body twice → one file, stable handle. |
| `Inv7EvictionAndSweep` | INV-7 — file cap holds, just-written handle survives; 24 h `spillSweep()` drops a backdated file. |
| `Inv9OffloadPrecedesRecordDispatch` | INV-9 — `mcp::offloadBody(` precedes `recordDispatch(` in the dispatch body; `read_spill` schema registered. |
| `Inv10ReadSpillWiring` | INV-10 — `registerToolProvider("read_spill"` with `CallerCwdContract::Optional` + `cmdReadSpill`; handler validates `^[0-9a-f]{64}$`. |
| `Inv11FailOpenWiring` | INV-11 — `offloadBody` returns the body on `ensurePrivateDir`/`commit` failure. |
| `Ants4397ShapeSummaryForLongRows` | ANTS-4397 — a body of few, very WIDE rows carries `rows_preview`: one `{index, bytes, head}` row per row, covering every row where the body prefix covered one. |
| `Ants4519HeadlessShapePreviewIsOmittedNotEmitted` | ANTS-4519 — when a text sample cannot fit for every row, `rows_preview` is omitted entirely (`rows_preview_omitted:true`, `row_count` kept) rather than emitted headless. A bare list of row LENGTHS conveys nothing `row_count` + `bytes` do not, and cost ~1k tokens on the session's most expensive calls. |
| `Ants4474NarrowsHeadsBeforeDroppingThem` | ANTS-4474 — the head gives way by SHRINKING before it gives way by vanishing. When a full-width sample cannot cover every row the width steps down (60 → 40 → 24 → 12) and the narrowest that covers them all is used, reported as `rows_preview_head_chars`. Only when no width covers every row does ANTS-4519's omission apply. A row label, a Markdown heading, a table cell name and a function signature all live in the first ~40 characters, so the narrowed head still answers "what is this row?" — the one question `bytes` cannot. |

## Pre-fix expectation

Against pre-feature code the bundle does not compile (`mcp::offloadBody`,
`mcp::readSpill`, `mcp::isOffloadEligible`, `read_spill` wiring all
absent) — the conformance is "feature present and wired", so the failing
state is a build/link error, then the behavioural asserts above.

Label: `features;fast`. Built into the `test_core` bundle (do NOT
`add_executable`).
| `Inv15PreservesTopLevelFields` | INV-15 (ANTS-4692) — the body's own top-level members survive an offload. A `roadmap_query`-shaped body keeps `source`, `file_in_sync`, `path` and `count`; the dominant array does not come back and is not an omission (`head_rows_key` names it); the envelope stays strictly smaller than the body. |
| `Inv15FloorCapOverrideAndCollision` | INV-15 — the four edges. A `*_checked:false` survives however much the ordinary members have spent (the protected floor), because dropping it reads as a clean pass over nothing examined. The floor does not lift the PER-MEMBER cap: an oversized protected key is still dropped, and NAMED in `preserved_omitted`. A body's `ok:false` overrides the envelope's placeholder. A body member named `handle` does not overwrite the spill handle. |
| `Inv15NoPreservationAboveParseCap` | INV-15 — a body over `kStructuredParseMaxBytes` is never parsed, so nothing is preserved AND no `preserved_omitted` is emitted. A guard against over-firing rather than a red-to-green case: it passes against pre-fix code too, because pre-fix preserved nothing anywhere. |
| `Inv15SkipsTheDispatcherAdvisory` | INV-15 — `ignored_args` is skipped, and the reason is pinned rather than only commented: it is the DISPATCHER's advisory, re-applied after the offload, and ANTS-4626's INV-2 asserts `offloadBody` drops it because that is what keeps its INV-1 an assertion about something. Asserts it is a skip (absent from `preserved_omitted`) and that an ordinary member beside it still survives, so this is a carve-out rather than preservation failing to run. |
