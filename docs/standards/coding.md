<!-- ants-coding-standards: 1 -->
# Coding Standards — v1

A shareable contract for code in this project. Pairs with the
other three standards in this folder ([documentation](documentation.md),
[testing](testing.md), [commits](commits.md)) — see the
[index](README.md) for the full set.

This standard governs ROADMAP bullets with `Kind: implement`,
`fix`, `refactor`, `audit-fix`, or `review-fix`. The other kinds
(`doc`, `test`, `chore`/`release`) defer to their respective
companion documents.


## 1. Principles

### 1.1 Shortest correct implementation

50 lines beats 250. No scaffolding for hypothetical futures, no
abstractions where a direct call works, no error paths for
scenarios that can't happen at the call site. Every line pays rent
in legibility or function.

### 1.2 No workarounds without a root-cause fix

Silencing warnings, `try/except: pass`, `--no-verify`, commenting
out broken code, disabling checks — last resort, not default.
Applies to build, test, runtime, and lint failures alike. When a
workaround is genuinely the only option, leave a comment naming
the underlying constraint so it reads as deliberate, not neglect.

### 1.3 Reuse before rewriting

Before writing new code, look for existing code that does the same
or similar thing, in order of preference:

1. Call it directly.
2. Refactor it to cover the new case, then call it — existing
   call-sites benefit.
3. Only if neither fits, write new code and justify the
   duplication.

**Rule of Three:** extract a helper on the third call-site, not
the first or second. Premature DRY costs more than duplication.

### 1.4 Six-month test

If someone opens this file six months from now, can they read the
change and understand *why* the code looks this way without the
author? If not, it's too clever or too long.

### 1.5 Use latest stable library + current idioms

When pulling in an external library (Qt, React, Python pkg, …),
prefer the latest stable release unless pinned for an explicit
reason. When calling library APIs, use the current idiomatic
syntax for that version — not the one current three years ago.

For per-language idiom examples (Qt 6, C++20+, Python 3.10+,
React 18+), see `~/.claude/CLAUDE.md § 5` — that's the
canonical source. When unsure what's current, check the
library docs first. Stale idioms compile but they age the
codebase.


## 2. Error handling

- **Validate at boundaries, not internally.** User input, network,
  IPC, deserialisation → validate. Internal calls → trust.
- **Don't write paths that can't happen.** If a function is only
  called with non-null input from internal code, don't add a null
  check.
- **Surface unexpected errors loudly.** Swallowed exceptions are
  loaded guns. Log + propagate, don't `except: pass`.
- **Specific exceptions over generic.** `except FileNotFoundError`
  over `except Exception`.
- **Don't write fallbacks for scenarios that can't occur.** Trust
  framework guarantees; only fall back at real failure points.


## 3. Comments

Default to **no comments**. Only add one when the WHY is
non-obvious:

- A hidden constraint (`// gpg is single-threaded; serialise here`).
- A subtle invariant (`// must run before m_grid is freed`).
- A workaround for a specific bug (`// QTBUG-79126: frameless +
  modal drops clicks on Wayland — fall back to event filter`).
- Behaviour that would surprise a reader.

Don't:

- Explain WHAT the code does — well-named identifiers do that.
- Reference the current task / fix / callers ("used by X", "added
  for Y") — those belong in the commit body.
- Write multi-line block comments or paragraph docstrings.


## 4. Naming

- **Functions** — verb phrases (`parseRGBColor`, `applyTheme`).
- **Variables** — noun phrases (`m_currentTab`, `gridSize`).
- **Booleans** — `is*` / `has*` / `can*` (`isReady`, `hasFocus`).
- **Constants** — match the file's existing style. Don't mix
  SCREAMING_SNAKE and PascalCase in one file.
- **Avoid abbreviations** except universally-known (`url`, `id`,
  `db`). Prefer `temperature` over `temp` when ambiguous.
- **No Hungarian notation.** `m_` prefix for member fields is
  fine where a project uses it; type prefixes (`strName`, `iCount`)
  are not.


## 5. Language-specific notes

### 5.1 C++

- C++20 minimum unless project pins higher.
- `auto` for obvious types; explicit type when the type matters
  for the reader.
- RAII for everything that owns a resource.
- `[[nodiscard]]` on factory / parser return types.
- `std::make_unique` / `std::make_shared` over raw `new`.
- Prefer `std::optional<T>` over sentinel values (`-1`, `nullptr`).
- `noexcept` on move constructors, swap, destructors.

### 5.2 Qt

- Modern signal-slot connection syntax only.
- Parent-child ownership; don't manually `delete` a parented child.
- `Q_OBJECT` macro on every QObject subclass.
- Wrap user-visible strings in `tr()` for translator compatibility.
- `QSaveFile` for atomic writes, not raw `QFile::Truncate`.
- `setOwnerOnlyPerms()` on files that contain config / secrets.

### 5.3 Python

- Type hints on every public function signature.
- Use `pathlib.Path` over `os.path`.
- `pyproject.toml` for config; no `setup.py`.
- `subprocess.run([cmd, arg])` not `shell=True` with f-strings.

(Add language sections as the project grows.)


## 6. Performance

- **Profile before optimising.** "Make it work, make it right,
  make it fast" — in that order.
- Avoid premature `O(n²)` patterns where `O(n)` fits.
- For hot paths: pre-allocate, batch I/O, avoid copies.
- Don't write a cache without measuring the hit rate first.
- Don't pessimise — use `std::move` on the return of
  rvalue-returning helpers, reserve capacity on growable
  containers when the size is known.


## 7. Security

- **Never trust user input.** Validate at the boundary.
- **No `shell=True`.** Use argv arrays:
  `subprocess.run([cmd, arg])`, `QProcess::start(cmd, args)`.
- **Atomic file writes.** Temp + rename, or `QSaveFile`. Don't
  truncate-and-write — a crash leaves an empty file.
- **Restrictive perms on secret-bearing files.** 0600 for config,
  tokens, keys.
- **Path traversal** — resolve and check `commonpath` /
  `QDir::canonicalPath` before opening user-supplied paths.
- **Argv injection** — when calling external tools with
  user-supplied filenames, prepend `--` separator and prefix
  paths with `./` if they could start with `-`.
- **Don't log secrets.** Strip Authorization headers, API tokens,
  private-key blocks before any `qDebug` / `print` / log call.


## 8. Anti-patterns

- ❌ Multi-paragraph docstrings on every function.
- ❌ "Just in case" exception handlers that swallow everything.
- ❌ Half-finished implementations behind feature flags.
- ❌ Renaming a variable to `_unused` instead of removing it.
- ❌ `// TODO: fix later` with no roadmap entry tracking it.
- ❌ Hardcoded paths / magic numbers without a named constant.
- ❌ Dead-code branches kept "just in case".
- ❌ Compatibility shims for callers that don't exist any more.
- ❌ `using namespace std;` in headers.
- ❌ `from foo import *` in Python.
