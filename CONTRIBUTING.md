# Contributing to Ants Terminal

Thanks for your interest in contributing! This document is a short,
actionable guide derived from [STANDARDS.md](STANDARDS.md) — the authoritative
source for style, architecture, and invariants. Start here, follow the links
for depth.

## Quick start

```bash
git clone https://github.com/milnet01/ants-terminal
cd ants-terminal
mkdir build && cd build
cmake .. -G Ninja
ninja
ctest --output-on-failure
./ants-terminal
```

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
.github/workflows/ CI: build + ctest + cppcheck + ASan/UBSan smoke test
STANDARDS.md       coding invariants (you are here's source of truth)
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
cmake --build build-asan --parallel
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

## Versioning + release

SemVer. Every version bump touches **three files**:

1. `CMakeLists.txt` — `project(... VERSION X.Y.Z)` (single source of truth).
2. `CHANGELOG.md` — new dated `## [X.Y.Z] — YYYY-MM-DD` section above the
   previous release. Categories: Added / Changed / Fixed / Security / Removed.
3. `README.md` — the "Current version: **X.Y.Z**" line.

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
0.6.7: three new audit rules — silent_catch, missing_build_flags, no_ci

Closes four ROADMAP 0.7.0 Dev-experience items. silent_catch matches
empty `catch(...){}` bodies; missing_build_flags nudges toward better
compile-time coverage; no_ci warns when a project has no CI config.
Also wires a sanitizer CI job so ASan/UBSan runs on every push.
```

## Code style highlights

- C++20, Qt6, K&R braces.
- `m_` for member vars, `s_` for static members, `PascalCase` types,
  `camelCase` functions.
- `#pragma once` everywhere; no include guards.
- Signals/slots for cross-component comms — never direct sibling calls.
- **No workarounds unless no viable solution exists.** Diagnose root cause
  first. Document the constraint if a workaround is genuinely the only
  option (see `STANDARDS.md` §Error Handling).

See `STANDARDS.md` for the full list.

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
