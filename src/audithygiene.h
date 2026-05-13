// Pure parsers that read project-local tool-calibration files and return
// the list of suppression tokens to pass through to the scanner tool.
//
// Split out from auditdialog.cpp so unit tests can exercise them without
// linking QtWidgets / network / the full dialog machinery. The AuditDialog
// methods `semgrepExcludeFlags()` / `banditSkipFlags()` delegate here.
//
// Origin: RetroDB audit-hygiene report 2026-04-21 — 1.3% signal rate on a
// codebase whose threat model was already documented in .semgrep.yml +
// pyproject.toml, because the runner wasn't reading those files.

#pragma once

#include <QString>
#include <QStringList>

namespace AuditHygiene {

// `.semgrep.yml` header block parser. Looks for a comment line containing
// "Excluded upstream rules", then collects any `#   <dotted.id>` comment
// lines that follow, stopping at the first non-comment line.
QStringList parseSemgrepExcludeRules(const QString &ymlText);

// `pyproject.toml` `[tool.ruff.lint.ignore]` parser. Finds the ruff section
// (prefers `[tool.ruff.lint]` over `[tool.ruff]`), locates its `ignore = [...]`
// (or `extend-ignore = [...]`) array, extracts `S<nnn>` codes, and maps them
// to `B<nnn>` codes (bandit's equivalent family).
QStringList parseBanditSkipCodes(const QString &pyprojectText);

// ANTS-1111 — probe `<projectPath>` for framework markers and return
// the list of recognised frameworks (in detection order, deduped).
// Currently recognised:
//   "flask"   — requirements.txt or pyproject.toml mentions "flask" on a line
//   "django"  — manage.py at projectPath OR "django" in those files
//   "react"   — package.json dependencies.react / .react-dom
//   "vue"     — package.json dependencies.vue
//   "qt6"     — CMakeLists.txt at root mentions Qt6:: or find_package(Qt6
//   "rust"    — Cargo.toml at root
//   "go"      — go.mod at root
// Empty list when none match. Pure file IO bounded to projectPath.
QStringList detectProjectFrameworks(const QString &projectPath);

// ANTS-1111 — map detected frameworks to the semgrep rule-pack args
// to splice into a QProcess::start argv list. Each framework
// contributes TWO consecutive entries (`{"--config", "p/flask"}`) so
// the caller can splice without re-tokenising. Unknown framework
// names contribute nothing. Recognised mappings: flask, django,
// react, vue.
QStringList semgrepRulePacks(const QStringList &frameworks);

} // namespace AuditHygiene
