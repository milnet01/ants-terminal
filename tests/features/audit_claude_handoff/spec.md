# The audit's Claude hand-off starts on real findings

The Audit dialog's Review-with-Claude button hands a Claude session the text
`AuditDialog::plainTextResults` builds. On 2026-11-21 that text sent a
session into a full build and test run for over 10 minutes. Its most serious
findings were all false alarms, and its clang-tidy section was mostly
progress lines. This contract pins the hand-off and the rules behind those
false alarms.

## Invariants

- **INV-1 — the baseline step is cheap** (ANTS-6855). The hand-off's baseline
  step does not ask the session to run the existing test suite. It says not
  to rebuild or run the whole suite first.
- **INV-2 — standards files are named, not pasted** (ANTS-6856). The hand-off
  names each project standards file that exists, under `=== Project rules ===`.
  A line of that file's text does not appear in it. A standards file the
  project lacks is not named.
- **INV-3 — clang-tidy reports only diagnostics** (ANTS-6857). The
  `clang_tidy` check's filter keeps `path:line:col: warning:` and `error:`
  lines and the lane's own `clang-tidy:` banners. It drops code snippets,
  caret lines, notes, `N warnings generated.`, `Suppressed N warnings` and
  `[1/1] Processing file` lines.
- **INV-8 — the cap counts warnings, not raw lines** (ANTS-6863). The
  `clang_tidy` command cuts no lines from clang-tidy's output before the
  filter runs, so 50 warnings with their snippet and caret lines give 50
  findings.
- **INV-4 — a public key is not an exposed key file** (ANTS-6858). The
  `env_files` check lists a `.pem` holding a private key and not one holding
  only a public key.
- **INV-5 — the same in git, and source names are not secrets**
  (ANTS-6858). The `git_sensitive` check lists a tracked private-key `.pem`,
  and neither a tracked public-key `.pem` nor a tracked `secretredact.h`.
- **INV-6 — a key read from settings is not a hardcoded secret**
  (ANTS-6858). `secrets_scan` does not fire on
  `req.apiKey = m_config->aiApiKey();`. It still fires on a quoted literal in
  C++ and on an unquoted value at the end of a YAML line.
- **INV-7 — `system()` with no argument is not command injection**
  (ANTS-6859). `cmd_injection` does not fire on
  `QRandomGenerator::system()->generate()`. It still fires on `system(cmd)`.

## Test

`test_audit_claude_handoff.cpp` in the `test_dialogs` bundle. It builds a
real `AuditDialog` over a scratch project. INV-1 and INV-2 read
`plainTextResultsForTest`. INV-3 runs the check's filter through
`AuditEngine::applyFilter` over captured clang-tidy output. INV-8 runs the
check's command with a stand-in `clang-tidy` first on `PATH`. INV-4 to INV-7
run each check's own command from the project folder with `/bin/bash -c`, as
`AuditDialog::runNextCheck` does, then its filter. Each failed against the
code before its item.
