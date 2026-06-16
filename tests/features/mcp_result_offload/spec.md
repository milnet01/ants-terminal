# mcp_result_offload — ANTS-2094 conformance

Test contract for proactive MCP result offload (observation masking).
Full design: `docs/specs/ANTS-2094.md`. Behavioural coverage of the
`mcp::` offload/spill API (in `ants_core_lib`) under an isolated test
cache dir (`QStandardPaths` test mode), plus source-scrapes for the
dispatch-site and verb-wiring invariants.

## Cases → invariants

| Test | Covers |
|---|---|
| `Inv1EligibilityAndRequestResolution` | INV-1 — eligible set is the large-body read verbs, separate from `isFieldProjectionTool`; write/control-plane never eligible; `offloadRequested` resolves per-call over session default. |
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

## Pre-fix expectation

Against pre-feature code the bundle does not compile (`mcp::offloadBody`,
`mcp::readSpill`, `mcp::isOffloadEligible`, `read_spill` wiring all
absent) — the conformance is "feature present and wired", so the failing
state is a build/link error, then the behavioural asserts above.

Label: `features;fast`. Built into the `test_core` bundle (do NOT
`add_executable`).
