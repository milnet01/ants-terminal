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
- **TF-5** (ANTS-5082) `saveToDisk` checks the write's byte count, the
  flush, the close and `setOwnerOnlyPerms` before the rename, and removes
  the temp file on any failure. Source-scrape: a full disk cannot be
  simulated in a unit test.
- **TF-6** (ANTS-5082) A corrupt trust file is renamed aside
  (`<path>.corrupt.<epoch>`, the ANTS-1179 naming) when loaded, keeping its bytes, before
  the next write creates a fresh file.
- **TF-4** (ANTS-1825) Schema-version gate. `saveToDisk` stamps
  `version`; `loadFromDisk` reads it. A file whose `version` exceeds
  `kSchemaVersion` (written by a newer Ants) is refused: no entries
  load (trust falls closed) and `saveToDisk` no-ops so the older
  client cannot downgrade-clobber the newer file. A missing `version`
  defaults to the current schema (back-compat with hand-edits).
- **TF-7** (ANTS-5082) `first_trusted` is the date an entry was first
  trusted. A save keeps the date each loaded SHA and repo entry carries,
  re-trusting an existing SHA keeps it too, and only a new entry gets a
  new date.
- **TF-8** (ANTS-5082, ANTS-1337 § 6) A trust file that group or other
  users can read or write is honoured on load, and `loadFromDisk` writes a
  warning naming the file to stderr. The next save narrows it to 0600
  (TF-1).
- **TF-9** A trust whose save fails is not honoured. `addTrustedSha` and
  `addTrustedRepo` roll the in-memory entry back when `saveToDisk` fails,
  so a later `outcomeForConfig` for that config is not `Trusted` in the
  same session. Exercised under TF-4's future-schema file, where every
  save refuses.

Phase 2 will add MD-* (modal) and MC-* (MCP envelope) tests.

MC-1..MC-4 are source-grep checks in this directory's test file, confirming
`cmdVerifyChangesImpl` reads `m_verifyTrustClient` and threads
`ANTS_VERIFY_TRUST_AUTOTRUST` through. **MC-5 lives in
`tests/features/mcpd_verify_trust_wiring/`**, which owns that contract: it
is a behavioural check (a real `ants-mcpd` child process) that the
`RemoteControl` instance serving `verify_changes` actually *has* a trust
client wired, not only that the code path that would use one exists.

## Invariants pinned for the trust prompt (ANTS-5082)

Source scrapes of `ModalClient::showPrompt`: the prompt is a modal
`QMessageBox`, which a unit test cannot drive.

- **MD-1** The prompt shows a `Gates:` line naming the gates the config
  would run (ANTS-1337 § 4.3).
- **MD-2** The prompt carries an "auto-re-prompt if the file changes"
  checkbox, checked by default, and "Trust this repo" passes its state to
  `addTrustedRepo` as `untilShaChanges`.
