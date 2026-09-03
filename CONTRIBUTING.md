# Contributing to Ants Terminal

Thanks for your interest in contributing! This document is a short,
actionable guide derived from [`docs/standards/`](docs/standards/) —
the authoritative source for style, architecture, and invariants
([coding](docs/standards/coding.md), [documentation](docs/standards/documentation.md),
[testing](docs/standards/testing.md), [commits](docs/standards/commits.md)).
Start here, follow the links for depth.

By participating in this project you agree to abide by the
[Code of Conduct](CODE_OF_CONDUCT.md) (Contributor Covenant 2.1).
Sensitive reports (security issues, conduct violations) go through the
private channels described in [`SECURITY.md`](SECURITY.md) and
[`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md) respectively — please don't
file them as public GitHub issues.

## Quick start

```bash
git clone https://github.com/milnet01/ants-terminal.git
cd ants-terminal
cmake -G Ninja -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/ants-terminal
```

For constrained hardware (or when the build is competing with a
heavy desktop session), use the `workstation` preset instead, which
hard-caps the build at `-j3`:

```bash
cmake --preset=workstation
cmake --build --preset=workstation
ctest --preset=workstation
```

Both invocations honour the in-tree `JOB_POOLS` cap that prevents
OOM kills under Ninja; plain `make` ignores the pool and is
discouraged for this project (see `CLAUDE.md` § Build & test).

Optional dependencies (`clazy`, `cppcheck`, `semgrep`, `lua5.4-devel`) unlock
extra audit checks and the plugin system. Each component probes with
`which <tool>` and self-disables if missing — builds never fail on an absent
optional dep.

## Where things live

```
src/               Qt widgets, VT parser, PTY handler, audit dialog
tests/             ctest drivers + fixtures
  audit_self_test.sh       regression harness for audit rule regexes
  audit_fixtures/<id>/     bad.*/good.* fixtures per rule
  features/                feature-conformance tests (spec.md + test_*.cpp)
.github/workflows/ CI: build + ctest + cppcheck + ASan/UBSan smoke test
docs/standards/    coding / documentation / testing / commits invariants
                   (you are here's source of truth)
PLUGINS.md         plugin API contract (update when ants.* changes)
ROADMAP.md         forward-looking plan; move items to CHANGELOG on ship
CHANGELOG.md       Keep-a-Changelog format, dated per-version sections
```

## Build modes

```bash
# Release (default, what CI builds)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

# Debug + ASan + UBSan (what the build-asan CI job runs)
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DANTS_SANITIZERS=ON
cmake --build build-asan
QT_QPA_PLATFORM=offscreen ASAN_OPTIONS=detect_leaks=0 ./build-asan/ants-terminal --version
```

Run the sanitizer build before sending a PR that touches initialization,
config load, command-line parsing, or VT parser state-machine code — the CI
job will catch regressions but local iteration is faster.

## Adding an audit rule

Every check registered in `src/auditdialog.cpp` via `addGrepCheck()` must
have both a fixture directory and a `run_rule` line in the test harness.
The `audit_fixture_coverage` check (and its CI-enforced twin in
`audit_self_test.sh`) blocks merges if either is missing.

1. **Register the check** in `src/auditdialog.cpp::populateChecks()`:

   ```cpp
   addGrepCheck("my_rule", "My Rule", "What it flags", "General",
                "'pattern-without-outer-quotes'",
                CheckType::CodeSmell, Severity::Minor, true);
   ```

2. **Create fixtures** under `tests/audit_fixtures/my_rule/`:

   - `bad.cpp` (or `.py`, `.sh`, etc.) — N lines with `// @expect my_rule`
     markers on every line the pattern should match.
   - `good.cpp` — lines that look similar but MUST NOT match. Common false
     positives: rule patterns mentioned in comments (use paraphrases or
     UPPER-CASE spellings since `grep -E` is case-sensitive by default).

3. **Register the test** in `tests/audit_self_test.sh`:

   ```bash
   run_rule "my_rule" '<regex exactly as passed to addGrepCheck>'
   ```

4. Run `ctest --output-on-failure`. The harness asserts:
   - `bad.*` produces exactly N matches (N = `@expect` marker count).
   - `good.*` produces zero matches.
   - Both fixture dir and run_rule line exist (fixture-coverage cross-check).

For custom-shell checks (those registered via `m_checks.append({...})`
rather than `addGrepCheck()`) the fixture-coverage rule does not apply —
they're scoped to the project, not to a code-pattern regex.

The grep-based rule path above covers the majority of checks. Two
additional paths exist for specialised work:

- **In-process checks** (e.g. `spec_code_drift`, `changelog_test_coverage`)
  are registered via `AuditCheck::inProcessRunner` in `src/auditengine.cpp`
  rather than `addGrepCheck()`. They run in-process without a QProcess shell
  and are not covered by the fixture-coverage harness.

- **Debt Sweep** (the _Debt Sweep_ tab in the Audit dialog) uses a
  separate triage→scan→apply pipeline in `src/auditengine.cpp` driven by
  `debt_sweep_scan` / `debt_sweep_triage_prompt` / `debt_sweep_apply_fix`
  MCP tools. Adding a new debt-sweep operation type requires extending
  `auditengine.cpp`'s repair-plan logic, not the grep-check registry.

## Versioning + release

SemVer. Every version bump runs `cut-release --bump-only`, which applies the
`.claude/bump.json` recipe to **all** version-bearing files;
`packaging/check-version-drift.sh` fails the build when one is missed. Never
hand-edit version strings individually.

Key files the bump touches:

1. `CMakeLists.txt` — `project(... VERSION X.Y.Z)` (single source of truth).
2. `README.md` — the header banner, `Version <strong>X.Y.Z</strong>`.
3. Packaging manifests (`packaging/opensuse/*.spec`, `packaging/archlinux/PKGBUILD`,
   etc.) — covered by the recipe.

`CHANGELOG.md`'s **version heading** is not the bump's: `packaging/cut-rc.sh
new-rc` rolls it and `promote` dates it. Bullets under the open
`[Unreleased]` section are authored as work lands.

When you ship a ROADMAP item, move it from `ROADMAP.md` (status `📋`) into
the matching `CHANGELOG.md` section. The `ROADMAP.md` entry converts to `✅`
with a link to the shipped CHANGELOG entry.

## Commit + PR conventions

- **One logical change per commit.** Rebase your branch before merge so the
  commit log reads as a story.
- **Imperative subject line**, under 70 characters.
- **Body explains the why**, not the what — reviewers can read the diff.
- **Pre-commit hooks must pass.** Never `--no-verify`; fix the underlying
  issue. (Same rule applies to CI.)
- **Reference ROADMAP or issue numbers** when applicable.

Example:

```
ANTS-1042: three new audit rules — silent_catch, missing_build_flags, no_ci

Closes four ROADMAP Dev-experience items. silent_catch matches empty
`catch(...){}` bodies; missing_build_flags nudges toward better
compile-time coverage; no_ci warns when a project has no CI config.
Also wires a sanitizer CI job so ASan/UBSan runs on every push.
```

(Per `docs/standards/commits.md` § 1.1: subject is `<ID>: <description>`
where `<ID>` is the stable ANTS-NNNN per `docs/standards/roadmap-format.md`
§ 3.5.1, or a literal version string for release commits like
`0.7.91:` only.)

## Code style highlights

- C++20, Qt6, K&R braces.
- `m_` for member vars, `s_` for static members, `PascalCase` types,
  `camelCase` functions.
- `#pragma once` everywhere; no include guards.
- Signals/slots for cross-component comms — never direct sibling calls.
- **No workarounds unless no viable solution exists.** Diagnose root cause
  first. Document the constraint if a workaround is genuinely the only
  option (global `coding.md` § 1.2).

See [`docs/standards/coding.md`](docs/standards/coding.md) for the full list.

## What not to send

- Backwards-compatibility shims. Delete obsolete code; don't leave
  `// removed` comments.
- Changes to `src/auditdialog.cpp` without matching fixture/test updates
  (the CI gate will reject).
- Changes that hardcode a version string anywhere except
  `CMakeLists.txt`'s `project(... VERSION ...)`.
- Qt-specific regex rules that duplicate clazy — clazy-standalone covers
  `findChild` misuse, connect-capture lifetime, old-style-connect, and
  container-inside-loop with far fewer false positives.

## Getting help

Open an issue with context (reproducer, expected vs actual, terminal
version + Qt version). For open-ended design questions, prefix the issue
title with `[rfc]` so maintainers can triage appropriately.
