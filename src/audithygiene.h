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

} // namespace AuditHygiene
