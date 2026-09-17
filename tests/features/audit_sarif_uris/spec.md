# audit_sarif_uris — SARIF export writes valid artifact URIs

**Bundle:** `test_audit` · **Suite:** `AuditSarifUris` · **Label:** `features;fast`

## Problem

`AuditDialog::exportSarif` wrote each finding's file path straight into
`artifactLocation.uri`, and the project path straight into
`invocations[].workingDirectory.uri`. SARIF 2.1.0 wants URIs there. A path
with a space, `#` or `%` produced a URI a consumer reads as a different file,
and a relative path carried no `uriBaseId`, so a consumer could not resolve it.

## Surface

- `AuditEngine::sarifArtifactLocation(file)` returns `{uri, uriBaseId}` for a
  relative path (percent-encoded, `uriBaseId` `%SRCROOT%`) and `{uri}` for an
  absolute path (a percent-encoded `file://` URI).
- `AuditEngine::sarifSrcRootUri(projectPath)` returns the `file://` URI of the
  project root, ending in `/`.
- `exportSarif` uses the first for every location, declares `%SRCROOT%` in
  `run.originalUriBaseIds` with the second, and uses the second for the
  working directory.

## Invariants

- **INV-1 — a relative path is encoded and anchored.**
  `sarifArtifactLocation("src/a b#1.cpp")` gives `uri` `src/a%20b%231.cpp`
  and `uriBaseId` `%SRCROOT%`.
- **INV-2 — an absolute path is a file URI.**
  `sarifArtifactLocation("/tmp/x y.cpp")` gives `uri` `file:///tmp/x%20y.cpp`
  and no `uriBaseId`.
- **INV-3 — the source root is a directory URI.**
  `sarifSrcRootUri("/home/u/my proj")` gives `file:///home/u/my%20proj/`.
- **INV-4 — the export uses both.** `exportSarif` calls
  `sarifArtifactLocation(`, writes `originalUriBaseIds`, and no longer assigns
  `f.file` to `artLoc["uri"]` directly.

## Out of scope

- `auditrunner.cpp`'s headless SARIF. `last_audit_summary` reads its URIs back
  as file paths, so encoding them there changes that verb's output.

## Regression history

- **ANTS-5084:** SARIF artifact URIs were not percent-encoded and relative
  ones lacked `uriBaseId`.
