<!-- ants-coding-standards: 2 -->
# Coding Standards — Ants Terminal deltas

> **The standard itself is `~/.claude/standards/coding.md`. Read it there.**
> This file carries only what is specific to *this* project — the C++/Qt
> spellings and the one project-local helper. It is not a copy and must never
> become one.

**Why this file is a delta (2026-08-12).** Until today it was a verbatim copy
of the global standard, dropped in by `/start-app` with an instruction to keep
it in sync that nothing checked. It had drifted since 2026-06-02, and by the
time it was reconciled it was instructing three things the current global
standard forbids — most sharply, it told you to add language sections *here*,
which is the exact growth global § 5 exists to prevent. Two copies are two
standards that will disagree; this one lost.

## Where the rules actually live

| You want | Read |
|---|---|
| Principles — shortest correct implementation, no workarounds, reuse before rewriting, the six-month test, latest stable library | global `coding.md` § 1 |
| State an assumption rather than building on it | global `coding.md` § 1.6 |
| Surgical changes — every changed line traces to the reason you are there | global `coding.md` § 1.7 |
| Error handling | global `coding.md` § 2 |
| **When a workaround is permitted** | global `coding.md` § 1.2 — *not* § 2 |
| Comments | global `coding.md` § 3 |
| Naming | global `coding.md` § 4 |
| Performance | global `coding.md` § 6 |
| Anti-patterns | global `coding.md` § 8 |
| Security — validate at the boundary, atomic writes, never log a credential | [`security.md`](security.md) — a verbatim mirror of the global owner, kept in-repo because this repo is public |
| **C++ spellings** — version floor, idioms | global `languages/cpp.md` |
| **Qt spellings** — `tr()`, `QSaveFile`, `Q_OBJECT`, parent-child ownership, new-style `connect` | global `languages/qt.md` |
| **Python spellings** — casing (`snake_case`, *not* the camelCase this file used to mandate), idioms | global `languages/python.md` |

**Adding a language?** Create `~/.claude/standards/languages/<name>.md`. Do
not add a section here, and do not add one to the global `coding.md` either.

## Project-local rules

These are the whole of this project's additions. Everything else is global.

### House style

- **K&R braces.**
- `m_` for member variables, **`s_` for static members**, `PascalCase` types,
  `camelCase` functions. (The `s_` prefix is the project-local part; the rest
  is `languages/qt.md`.)
- **`#pragma once` everywhere; no include guards.**
- **Signals/slots for cross-component communication — never a direct call
  between siblings**, where *sibling* means two components neither of which
  owns the other. A component calling something it owns (`m_grid->…`) is not
  a sibling call and is not a breach — there are 244 such call sites. This is
  the architectural rule behind the module map in
  [`docs/subsystems.md`](../subsystems.md); a direct sibling call couples two
  lanes that are meant to be independently reviewable.

These four were previously stated only in
[`CONTRIBUTING.md`](../../CONTRIBUTING.md) § Code style highlights, which
pointed here "for the full list" — at a file that did not contain them.

### `setOwnerOnlyPerms()` is ours, not Qt's

Global `languages/qt.md` § Idioms lists "`setOwnerOnlyPerms()` on any file
holding config or secrets" beside `tr()` and `QSaveFile`, as though it were a
Qt API. **It is ours**, defined in
[`src/secureio.h`](../../src/secureio.h) — not a Qt built-in, and not
available in any other Qt project. It has two overloads
(`QFileDevice&` and `const QString&`). Reach for it rather than hand-rolling
a `QFile::setPermissions()` call, and do not go looking for it in the Qt
documentation.

## What checks this

Nothing mechanical checks the house-style rules; they are read in review.

`setOwnerOnlyPerms()` is **partially** checked. Hand-rolling the bitmask is
caught: `setPermissions_pair_no_helper` is a hardcoded grep check
(`src/auditdialog.cpp`, not `audit_rules.json`) with fixtures at
`tests/audit_fixtures/setPermissions_pair_no_helper/` and a row in
`tests/audit_self_test.sh`. **Nothing** catches the other half — a config
writer that never sets permissions at all compiles and passes.

## Review loop log

| Loop | Date | Lanes | Q1/Q2/Q3/Q4 | Outcome |
|---|---|---|---|---|
| 1 | 2026-08-12 | 1 (cold, general-purpose) | Q1 2 · Q2 2 · Q3 1 · Q4 n/a | 5 verified, 5 fixed, 0 dismissed. casing routed to cpp.md where cpp.md itself defers Qt projects to qt.md; `What checks this` claimed nothing covers `setOwnerOnlyPerms()` when `setPermissions_pair_no_helper` is a fixture-backed grep check; the qt.md delta was stated backwards; "sibling" was undefined against 244 owner-to-owned call sites; CONTRIBUTING's workaround pointer landed on § 2 rather than § 1.2 (fixed there). |
