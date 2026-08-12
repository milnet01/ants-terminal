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
| Comments | global `coding.md` § 3 |
| Naming | global `coding.md` § 4 |
| Performance | global `coding.md` § 6 |
| Anti-patterns | global `coding.md` § 8 |
| Security — validate at the boundary, atomic writes, never log a credential | global `security.md` (there is no project copy; read the global file) |
| **C++ spellings** — version floor, idioms, casing | global `languages/cpp.md` |
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
  between siblings.** This is the architectural rule behind the module map in
  [`docs/subsystems.md`](../subsystems.md); a direct sibling call couples two
  lanes that are meant to be independently reviewable.

These four were previously stated only in
[`CONTRIBUTING.md`](../../CONTRIBUTING.md) § Code style highlights, which
pointed here "for the full list" — at a file that did not contain them.

### `setOwnerOnlyPerms()` is ours, not Qt's

Global `languages/qt.md` requires owner-only permissions on any file holding
config or secrets. In this project the helper that does it is
**`setOwnerOnlyPerms()` in [`src/secureio.h`](../../src/secureio.h)** — a
project-local function, **not** a Qt built-in. It has two overloads
(`QFileDevice&` and `const QString&`). Reach for it rather than hand-rolling
a `QFile::setPermissions()` call, and do not go looking for it in the Qt
documentation.

## What checks this

Nothing mechanical checks the house-style rules; they are read in review.
`setOwnerOnlyPerms()` usage is not enforced either — a new config writer that
skips it compiles and passes. The `/audit` pack's grep rules are the closest
thing, and they do not cover this.
