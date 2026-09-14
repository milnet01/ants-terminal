# Feature: the audit cache directory is created private

## Invariants

**INV-1 — no audit writer creates `.audit_cache` with `mkpath`.** The SARIF
and HTML export buttons in `src/auditdialog.cpp` and
`writeGitleaksExcludeConfig` in `src/auditrunner.cpp` create the directory
with `ensurePrivateDir`, which makes it 0700 with no umask window.

## Rationale

ANTS-1988 moved the cache writers to `ensurePrivateDir`. The ANTS-5085
performance pass found three that still called `QDir().mkpath` first, so the
directory could exist at umask permissions before a later call tightened it.

## Test surface

`test_audit_cache_dir_private.cpp` reads both sources (located from the test's
own path) and checks that no `mkpath` call names the cache directory.

## Regression history

- **ANTS-5085:** the three `mkpath` calls above. Locked by this spec.
