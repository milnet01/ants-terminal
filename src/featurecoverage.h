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
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

namespace FeatureCoverage {

// ---------------------------------------------------------------------------
// Lane 1 — spec ↔ code drift
// ---------------------------------------------------------------------------

struct SpecToken {
    QString token;
    int line{};  // 1-based line in the spec body where the token appears
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

// ANTS-3600 — a fence-aware, path-widened variant of extractSpecTokens for
// the contract-doc drift lane. Two differences from extractSpecTokens:
//   1. the token charset adds `/` so slash-paths (`archived/unknown`,
//      `tools/e2e/cases.sh`) are captured;
//   2. lines inside fenced code blocks are skipped (a fenced example is an
//      illustration, not a contract claim) — fence state comes from the
//      shared MarkdownScan scanner, and back-ticked URLs are dropped.
// Distinct function so the existing tests/features drift lane's token shape is
// untouched. See docs/specs/ANTS-3600.md § 2.4.
QList<SpecToken> extractDocLiteralTokens(const QString &docText);

// ---------------------------------------------------------------------------
// Lane 2 — CHANGELOG ↔ feature-test coverage
// ---------------------------------------------------------------------------

struct ChangelogBullet {
    QString section;  // e.g. "Added", "Fixed"; empty if no ### header seen yet
    QString text;     // bullet body with leading "- " stripped
    int line{};         // 1-based line in CHANGELOG where the bullet starts
};

// Parse the topmost `## ...` version section of a Keep-a-Changelog-style
// body. Emits one `ChangelogBullet` per `- `-prefixed line, tagged with
// its enclosing `### Subsection` (if any). Returns empty list if the body
// has no `## ` header at all. Bullets whose body spans multiple lines
// are represented by their first line only — the first line is typically
// the human-facing summary and the continuation is prose detail.
// ANTS-2007 — skipUnreleased=true anchors on the first RELEASED version,
// skipping `## [Unreleased]`; used by the coverage check so in-progress
// (unreleased, not-yet-specced) items aren't flagged as missing a test. The
// default (false) returns the top section verbatim, Unreleased or not.
QList<ChangelogBullet> extractTopVersionBullets(const QString &changelogText,
                                                bool skipUnreleased = false);

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

// ANTS-4099 — the trailing parenthesised ticket id of a CHANGELOG bullet
// (`**Thing that shipped** (FIBR-0042)` → `FIBR-0042`), or "" when there
// is none. Only a TRAILING id counts: that is the position `changelog_log`
// writes it in, and an id quoted mid-sentence is a cross-reference to some
// other entry, not this bullet's key.
QString extractChangelogEntryId(const QString &bulletText);

// ---------------------------------------------------------------------------
// Reusable project-walk helpers (ANTS-1113 v1).
//
// Lifted out of runSpecDriftCheck so DebtSweepEngine and other
// downstream callers (audit-rule writers, future MCP tools) can reuse
// the same blob-builder + identifier-existence check without copying
// the extension list, skip-dir set, or fallback rules.
//
// Pure Qt::Core; safe to call from any thread that owns its own QFile
// handles. No caching layer — caller decides whether to memoise.
// ---------------------------------------------------------------------------

// Concatenate every source/config/doc file in the project tree (per the
// canonical extension list) into one UTF-8 blob, separated by newlines.
// Skips: build dirs (.git, .svn, build, build-*, dist, node_modules,
// __pycache__, .venv, venv, .audit_cache, .pytest_cache, .mypy_cache,
// .tox, target, .claude, vendor, third_party, external, .ccls-cache),
// `spec.md` files (would self-match every spec token), and any file not
// matching the extension list. Returns the empty string if projectPath
// doesn't exist or contains no matching files.
//
// ANTS-2113 — the walk is deliberately whole-tree (NOT src/-only): a feature
// spec.md legitimately cites its own gtest case names, which live only in
// tests/, plus package names / manifest keys that live at the repo root.
// Excluding tests/ to make the drift lane's old "no match in src/" message
// literally true was tried and floods the lane with false drift on every
// spec-cited test-case name (verified ANTS-2113), so the message — not the
// scope — was the bug.
// ANTS-3600 — options controlling what buildProjectSourceBlob concatenates.
// The defaults reproduce the pre-ANTS-3600 whole-tree-including-`.md`
// behaviour, which is what the remaining 1-arg call-site (DebtSweepEngine)
// still wants. ANTS-4098 moved runSpecDriftCheck onto the 2-arg form so it
// gets the path manifest too.
struct BlobOptions {
    bool includeMarkdownContents = true;   // false → exclude *.md bodies
    bool appendPathManifest      = false;  // true  → append every walked
                                           //         file's project-relative
                                           //         path (before all gates)
};

QString buildProjectSourceBlob(const QString &projectPath,
                               const BlobOptions &opts = {});

// Does `token` appear anywhere in `blob`? Substring containment first;
// then `::` and `.` tail-fallbacks (so `Class::method` and `module.func`
// resolve to the trailing identifier alone when the compound form
// isn't present in the blob). The `.` fallback only applies when the
// tail is identifier-shaped (≥4 alpha chars) — prevents matching file
// extensions like `*.cpp` → `cpp`. (ANTS-2113 H2 weighed identifier-boundary
// matching here but deferred it — on this repo it surfaces imprecise spec
// wording, not real drift; see ROADMAP.)
bool existsInSource(const QString &blob, const QString &token);

// Public accessor for the existing internal `kSpecStopwords` set
// (shared with extractSpecTokens). Lets DebtSweepEngine drop the same
// stop-words from its bare-comment-token detector without re-declaring
// the literal list.
const QSet<QString> &specStopwords();

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
// extracts identifier tokens, reports the ones that match nothing in the
// whole-tree blob — neither any file's text nor (ANTS-4098) any file's
// project-relative path, so a spec citing a file that exists is not drift.
// Silently returns "" for projects without the `tests/features/` convention
// or without a `src/` tree.
QString runSpecDriftCheck(const QString &projectPath);

// Lane 2 — CHANGELOG bullets with no locking feature test. Reads
// `<projectPath>/CHANGELOG.md` and reports bullets in the top released
// version's Added/Fixed subsections that match nothing in
// `<projectPath>/tests/features/*`. A bullet is matched two ways, in order:
// by its trailing `(PROJ-NNNN)` id appearing anywhere in that corpus
// (ANTS-4099 — the exact join key), else by `bulletMatchesAnyTitle` against
// the specs' H1 titles. Silently returns "" if CHANGELOG.md is absent or no
// specs exist.
QString runChangelogCoverageCheck(const QString &projectPath);

// ANTS-3600 — read `<projectPath>/.ants_doc_drift_allow.txt` into a token
// set (one literal per line; whole-line `#` comments and blank lines
// ignored; each token trimmed of surrounding whitespace / CRLF). Absent
// file → empty set. Suppresses a token by exact, case-sensitive equality.
QSet<QString> loadAllowlist(const QString &projectPath);

// ANTS-3600 — Lane 3: contract-doc ↔ code literal drift. For each `*.md`
// under `<projectPath>/docs/standards/` and `docs/specs/`, extract
// back-ticked literals (extractDocLiteralTokens) and report the ones that
// appear in no non-`.md` source file and match no real file path in the
// tree (buildProjectSourceBlob with markdown bodies excluded + a path
// manifest). Allowlisted tokens (loadAllowlist) are suppressed. Silent
// no-op ("") for a project with neither docs dir. A pure free function,
// dispatched by the GUI Audit dialog's inProcessRunner path — see
// docs/specs/ANTS-3600.md.
QString runContractDocDriftCheck(const QString &projectPath);

} // namespace FeatureCoverage
