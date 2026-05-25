# Feature: verify-trust gate

Canonical design: `docs/specs/ANTS-1337.md`. This is the test-side
restatement for Phase 1 (infrastructure + engine API, no modal).

## Problem

`VerifyEngine::loadGateConfig` reads `.ants/verify.json` from the
project root and feeds its `command` strings to `/bin/sh -c` via
`runOneGate`. A hostile cloned repo with a malicious `command` runs
arbitrary shell the first time the user fires `mcp__ants__verify_changes`
from inside it.

Fix (Phase 1): add a `VerifyTrust::Client` interface + a
`FilePersistedTrustClient` impl that loads/saves trust decisions to
`~/.config/ants-terminal/verify-trust.json` (mode 0600 atomic).
Extend `loadGateConfig` to optionally consult the client; when the
SHA isn't trusted, fall back to auto-detect and surface
`verifyUntrusted=true`. Phase 1 ships purely additive — existing
callers (nullptr client) see no behavior change. Phase 2 wires
`cmdVerifyChanges` to pass a real client + adds the modal.

## Invariants pinned by Phase 1 tests

- **VT-1** Trusted SHA honoured → bespoke gates returned;
  `verifyUntrusted=false`.
- **VT-2** Untrusted SHA (AlwaysDenyClient) → auto-detect gates;
  `verifyUntrusted=true`; `configSource` = "auto (untrusted-bespoke)".
- **VT-6** Headless (null client) → bespoke honoured (Phase-1
  back-compat); `verifyUntrusted=false`.
- **TF-1** Trust file written with mode 0600.
- **TF-2** Atomic rewrite — interrupted write doesn't leave a
  half-written file (test simulates via direct rename check).
- **TF-3** Corrupt JSON tolerated — bad file loads as empty trust
  set; next write replaces it.
- **TF-4** (ANTS-1825) Schema-version gate. `saveToDisk` stamps
  `version`; `loadFromDisk` reads it. A file whose `version` exceeds
  `kSchemaVersion` (written by a newer Ants) is refused: no entries
  load (trust falls closed) and `saveToDisk` no-ops so the older
  client cannot downgrade-clobber the newer file. A missing `version`
  defaults to the current schema (back-compat with hand-edits).

Phase 2 will add MD-* (modal) and MC-* (MCP envelope) tests.
