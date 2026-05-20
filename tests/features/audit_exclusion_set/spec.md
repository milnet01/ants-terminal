# Audit directory-exclusion set — single source of truth (ANTS-1709)

Continues ANTS-1707 (Project Audit false-positive reduction). The set of
directories every audit scanner must skip — VCS, vendored code, language
caches, our own artifact dirs, and the `build*` glob family — was copied
across five sites:

- `find -not -path …`        (the `kFindExcl` shell expression)
- `grep --exclude-dir=…`     (the `kGrepExcl` shell expression)
- `trivy --skip-dirs …`      (a hardcoded CSV)
- `cppcheck -i …`            (a `for d in build build-* …` runtime loop)
- `FeatureCoverage` walk     (the `kSkipTopDirs` `QSet`)

The copies drifted. trivy's static CSV listed `build,build-test,
build-release,build-asan` and silently missed `build-fast` /
`build-workstation` — the **same false-positive class ANTS-1707 fixed
for cppcheck** (a static list scans the vendored googletest tree under a
build preset it doesn't know about).

This bullet consolidates the set into `AuditEngine` (Qt6::Core-only, so
the headless `audit_run` path can share it later — ANTS-1706) and
re-points every consumer at per-tool formatters. It also teaches the
`qt_openurl_unchecked` grep rule the `QUrl::fromLocalFile()` idiom — the
named residual false-positive class (a `file://` URL built from a
trusted local path needs no scheme gate).

## Surface

- `AuditEngine::excludedDirNames()` — canonical dir-name list. `build`
  is **not** in it (it expands to a glob family, formatted per-tool).
- `AuditEngine::findExcludeExpr()` — find `-not -path` form.
- `AuditEngine::grepExcludeExpr()` — grep `--exclude-dir` form.
- `AuditEngine::trivySkipDirsCsv()` — comma-joined, glob-based.
- `AuditEngine::cppcheckIgnoreShellExpr()` — runtime `build*` expansion.
- `src/auditdialog.cpp`, `src/featurecoverage.cpp` — consumers.

## Invariants

- **INV-1** `excludedDirNames()` carries the canonical cache/vendor dirs
  (`node_modules`, `vendor`, `.audit_cache`, `__pycache__`, `Testing`,
  `.git`, `.venv`) and **omits** `build` (globbed separately).
- **INV-2** `findExcludeExpr()` carries both `./build/*` and
  `./build-*/*` so every build preset is skipped, and matches
  `*/__pycache__/*` at any depth.
- **INV-3** `grepExcludeExpr()` carries `--exclude-dir=build` and
  `--exclude-dir='build-*'`.
- **INV-4** `trivySkipDirsCsv()` is glob-based — contains `build-*` and
  no longer carries the static `build-test,build-release,build-asan`
  enumeration. Locks the ANTS-1707-class regression.
- **INV-5** `cppcheckIgnoreShellExpr()` expands `build*` at run time
  (`for d in build build-* …` + `printf -- '-i %s '`).
- **INV-6** `auditdialog.cpp` routes find / grep / trivy / cppcheck
  exclusions through `AuditEngine` — no hardcoded `--skip-dirs
  build,build-test…` literal and no inline `for d in build build-*
  node_modules .audit_cache` loop remain.
- **INV-7** `featurecoverage.cpp` derives `kSkipTopDirs` from
  `AuditEngine::excludedDirNames()` (no hand-maintained duplicate list).
- **INV-8** the `qt_openurl_unchecked` rule recognises `fromLocalFile` /
  `QUrl::fromLocalFile` as a validated-source idiom.
- **INV-9** `AuditEngine::applyFilter` drops an `openUrl` finding line
  containing `fromLocalFile`.
- **INV-10** (ANTS-1707 regression lock) the `header_guards` rule probes
  `#pragma once` across the whole file *before* the `head`-limited
  `#ifndef` probe — so a guard sitting behind a long top-of-file doc
  comment is no longer misreported as "unguarded".

## ANTS-1710 — grep-rule idiom-blind-spot tightenings

The framework-awareness sweep deferred from ANTS-1709. All 25 hardcoded
`addGrepCheck`/`addFindCheck` rules were reviewed; three carried idiom
blind spots that produced false positives on correct, idiomatic code:

- **INV-11** the `insecure_http` rule drops license-header / spec URLs
  (`apache.org/licenses`, `gnu.org/licenses`) — documentation strings,
  not live insecure endpoints. These appear in nearly every source tree
  and were the dominant `insecure_http` FP.
- **INV-12** `AuditEngine::applyFilter` drops an `http://` finding line
  containing a license-header URL host.
- **INV-13** the `secrets_scan` rule drops the env-read idiom
  (`qEnvironmentVariable`, `getenv`, `qgetenv`): reading a secret *from*
  the environment is the safe pattern, but the call expression trips the
  rule's ≥16-char value arm.
- **INV-14** `AuditEngine::applyFilter` drops a `password =
  qEnvironmentVariable(...)` finding line.

The third tightening — `hardcoded_ips` constraining each group to a valid
octet (0-255) so build numbers like `1.2.300.4` no longer false-match — is
a **regex** change, locked by `tests/audit_self_test.sh` +
`tests/audit_fixtures/hardcoded_ips/` rather than here.
