// Pure parsers and fuzzy matchers for the feature-coverage audit lanes.
//
// Split out from auditdialog.cpp so the parsers can be driven from a
// headless feature test without linking QtWidgets or the full dialog
// machinery. The AuditDialog callbacks that run the actual checks do
// the file I/O and compose these functions.
//
// Two independent lanes live here:
//
//   Lane 1 — spec ↔ code drift. For each `tests/features/*/spec.md`,
//   extract backtick-fenced identifier-shaped tokens; the caller
//   verifies each still exists somewhere under `src/`. Catches the
//   common case where a rename obsoletes the spec prose silently.
//
//   Lane 2 — CHANGELOG ↔ feature-test coverage. Parse the top
//   `## [x.y.z]` section of a Keep-a-Changelog-formatted file, group
//   bullets by their `### Subsection`, and for each Added/Fixed
//   bullet ask whether any feature-test spec title matches. Surfaces
//   release-note claims that never got a locking test.

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace FeatureCoverage {

// ---------------------------------------------------------------------------
// Lane 1 — spec ↔ code drift
// ---------------------------------------------------------------------------

struct SpecToken {
    QString token;
    int line;  // 1-based line in the spec body where the token appears
};

// Extract backtick-fenced identifier-shaped tokens from a markdown body.
//
// A "token" is content between backticks that matches
// `[A-Za-z_][A-Za-z0-9_:\-\.]{3,}` — CamelCase, snake_case, dotted.ids,
// scoped::names, kebab-case are all accepted; pure numbers, prose
// fragments with whitespace, and short tokens are rejected.
//
// Tokens in `kSpecStopwords` (ubiquitous type/keyword names that would
// never usefully drift) are dropped. Duplicates are collapsed to the
// first-seen line — the caller reports drift once per token, not once
// per mention.
QList<SpecToken> extractSpecTokens(const QString &specText);

// Drive `extractSpecTokens` and filter through the caller-supplied
// `existsInSource` predicate. Returns only the tokens for which the
// predicate returned false — i.e. the "drifted" references that the
// spec still talks about but `src/` no longer contains.
QList<SpecToken> findDriftTokens(
    const QString &specText,
    const std::function<bool(const QString &)> &existsInSource);

// ---------------------------------------------------------------------------
// Lane 2 — CHANGELOG ↔ feature-test coverage
// ---------------------------------------------------------------------------

struct ChangelogBullet {
    QString section;  // e.g. "Added", "Fixed"; empty if no ### header seen yet
    QString text;     // bullet body with leading "- " stripped
    int line;         // 1-based line in CHANGELOG where the bullet starts
};

// Parse the topmost `## ...` version section of a Keep-a-Changelog-style
// body. Emits one `ChangelogBullet` per `- `-prefixed line, tagged with
// its enclosing `### Subsection` (if any). Returns empty list if the body
// has no `## ` header at all. Bullets whose body spans multiple lines
// are represented by their first line only — the first line is typically
// the human-facing summary and the continuation is prose detail.
QList<ChangelogBullet> extractTopVersionBullets(const QString &changelogText);

// Extract "significant" lowercase words from a sentence — tokenize on
// non-alphanumeric (treating `_` and `-` as word-internal), lowercase,
// drop English stopwords, drop words < 4 chars. Used by the fuzzy
// matcher's fallback path.
QStringList significantWords(const QString &text);

// Extract backtick-fenced tokens from a string, filtered to
// identifier-shaped (no whitespace, len ≥ 3) entries. Shared by both
// lanes — the CHANGELOG matcher uses this to find feature names like
// `launch`, `new-tab`, `RemoteControl::dispatch` inside a bullet.
QList<QString> extractBacktickTokens(const QString &s);

// Does `bulletText` plausibly correspond to any spec title in `titles`?
//
// Match rules (first one that fires wins):
//   1. Strong — any backtick-fenced token in the bullet equals any
//      backtick-fenced token in a title.
//   2. Fallback — ≥2 `significantWords()` of the bullet's first 120
//      characters appear in the title's words.
//
// Returns false when both paths fail → the bullet is an untested
// release-note claim.
bool bulletMatchesAnyTitle(const QString &bulletText,
                           const QStringList &specTitles);

// ---------------------------------------------------------------------------
// File-I/O runners — compose the pure parsers above with on-disk project
// layout. Return `file:line: message` stdout exactly as a shell-based
// audit check would, so the downstream `parseFindings` / dedup / suppress
// pipeline in auditdialog.cpp handles them uniformly.
//
// Factored out of AuditDialog so headless test-drivers and the audit
// dialog share one implementation. Only Qt::Core is required — no
// QtWidgets, no network, no QApplication.
// ---------------------------------------------------------------------------

// Lane 1 — spec ↔ code drift. Walks `<projectPath>/tests/features/*/spec.md`,
// extracts identifier tokens, reports the ones not present anywhere under
// `<projectPath>/src/`. Silently returns "" for projects without the
// `tests/features/` convention or without a `src/` tree.
QString runSpecDriftCheck(const QString &projectPath);

// Lane 2 — CHANGELOG bullets without a matching feature-test spec title.
// Reads `<projectPath>/CHANGELOG.md`, collects the spec titles from
// `<projectPath>/tests/features/*/spec.md`, and reports bullets in the
// top version section's Added/Fixed subsections that don't match any
// title via `bulletMatchesAnyTitle`. Silently returns "" if CHANGELOG.md
// is absent or no specs exist.
QString runChangelogCoverageCheck(const QString &projectPath);

} // namespace FeatureCoverage
