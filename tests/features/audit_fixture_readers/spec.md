# audit_fixture_readers — both audit rule-id readers see one set

**Parent spec:** [`docs/specs/ANTS-1677-large-file-decomposition.md`](../../../docs/specs/ANTS-1677-large-file-decomposition.md) § 3, INV-5
**Bundle:** `test_dialogs` · **Suite:** `AuditFixtureReaders` · **Label:** `features;fast`

## Why this exists

Two readers extract every `addGrepCheck("<id>"` from the AuditDialog source:

- the fixture-coverage block of `tests/audit_self_test.sh`;
- the product's `audit_fixture_coverage` check, built in
  `AuditDialog::populateChecks()`.

Both stay silent when they extract nothing. ANTS-1044 moves the calls into
another file of the class. A reader left on the old file then passes while
checking nothing.

## Invariant

- **INV-5 — `SelfTestAndRuntimeCheckSeeTheSameIds`.** Over one copy of the
  tree, `tests/audit_self_test.sh --list-rule-ids` and the
  `audit_fixture_coverage` command return the same id set. Neither set is
  empty. The command comes from `AuditDialog::checksForTest()`, so the case
  runs the catalogue's own text.
  *Breaks when* a reader reads a file the calls no longer live in.

## Why a copy of the tree

The runtime check reports only ids that lack a fixture directory. The real
tree has a fixture for every id, so the check would report nothing there. The
copy holds the class's sources and the script, and no `tests/audit_fixtures/`.

The copy takes the class's files by the shell readers' glob:
`src/auditdialog.cpp`, then `src/auditdialog_*.cpp`.

## Bundle

`test_dialogs`, not `test_audit`. The case constructs an `AuditDialog`, which
the engine-only `test_audit` bundle cannot link.

## Must-fail-first

A scratch `tests/audit_self_test.sh` whose extraction pattern matches nothing
turns the case red on the empty-set assertion. The case reads the script at run
time, so that mutation needs no rebuild.
