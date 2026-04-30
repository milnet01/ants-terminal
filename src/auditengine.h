#pragma once

// AuditEngine — Qt6::Core-only data types + pure-function helpers
// extracted from auditdialog.cpp per ANTS-1119. The audit dialog is
// a thin presentation layer over this module; non-GUI consumers
// (CI runners, future ants-helper audit-run, MCP server) link
// auditengine.cpp directly without dragging Qt6::Widgets in.
//
// **v1 scope (ANTS-1119 v1):** the pure-data shape (CheckType,
// Severity, OutputFilter, Finding, CheckResult, AuditCheck) plus
// three demonstrably-pure parsing functions (`applyFilter`,
// `parseFindings`, `capFindings`). Subsequent function migrations
// land under follow-up bullets — see ANTS-1119 spec § Open
// questions, § Out of scope.
//
// All types and functions in this header are usable with Qt6::Core
// only; no QWidget, QPainter, QDialog references.

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

class ToggleSwitch;  // Forward decl only — declared here for the
                     // AuditCheck.toggle pointer field. The widget
                     // type itself is GUI-side; engine consumers
                     // never dereference it.

// SonarQube-style taxonomy (informational tag for the UI + summary). A single
// check is exactly one of these — the category string (General, Security,
// C/C++, …) is orthogonal and groups checks for display.
enum class CheckType {
    Info,          // Informational counter (line count, large files, …)
    CodeSmell,     // Maintainability concern; not a bug per se
    Bug,           // Code that is demonstrably wrong or likely wrong
    Hotspot,       // Security-sensitive; needs human judgement
    Vulnerability, // Exploitable flaw
};

// SARIF-compatible severity; matches SonarQube's ordering.
enum class Severity {
    Info     = 0,  // FYI, no expected impact
    Minor    = 1,  // Low impact
    Major    = 2,  // Medium impact
    Critical = 3,  // High impact or a security flaw
    Blocker  = 4,  // Severe unintended consequences — fix immediately
};

// Declarative, line-level output filtering. Applied in C++ AFTER the shell
// command returns so filter logic stays in the source instead of buried in
// six-grep pipelines. Keeps individual checks readable and testable.
struct OutputFilter {
    QStringList dropIfContains;
    QString dropIfMatches;
    QStringList keepOnlyIfContains;
    int maxLines = 100;
    QStringList dropIfContextContains{};
    int contextWindow = 5;
};

struct AuditCheck {
    QString id;
    QString name;
    QString description;
    QString category;
    QString command;
    CheckType type = CheckType::CodeSmell;
    Severity severity = Severity::Minor;
    OutputFilter filter;
    bool autoSelect = false;
    bool available = true;
    ToggleSwitch *toggle = nullptr;  // GUI-side; engine never derefs
    std::function<QString(const QString & /*projectPath*/)> inProcessRunner{};
    int timeoutMs = 30000;
};

struct Finding {
    QString checkId;
    QString checkName;
    QString category;
    CheckType type;
    Severity severity;
    QString source;
    QString file;
    int     line = -1;
    QString message;
    QString dedupKey;
    bool    highConfidence = false;
    bool    suppressed = false;
    int     confidence = -1;
    QString snippet;
    int     snippetStart = 0;
    QString blameAuthor;
    QString blameDate;
    QString blameSha;
    QString aiVerdict;
    int     aiConfidence = -1;
    QString aiReasoning;
};

struct CheckResult {
    QString checkId;
    QString checkName;
    QString category;
    CheckType type;
    Severity severity;
    QString source;
    QList<Finding> findings;
    int omittedCount = 0;
    QString output;
    int findingCount = 0;
    bool warning = false;
};

namespace AuditEngine {

// FilterResult mirrors the AuditDialog::FilterResult shape (was a
// nested struct before extraction). Returned by applyFilter.
struct FilterResult {
    QString body;
    int count;
};

// Pure-function counterpart of `AuditDialog::applyFilter`. Takes
// `projectPath` so the context-window file lookup (parsing
// `./relative.cpp:LINE:` references in checker output) works without
// member access. Behaviour byte-identical to the prior method —
// ANTS-1119 INV-3 is locked by the audit_rule_fixtures regression
// suite which exercises this path end-to-end.
FilterResult applyFilter(const QString &raw,
                         const OutputFilter &f,
                         const QString &projectPath);

// Pure-function counterpart of `AuditDialog::parseFindings`. Was
// already static; signature is unchanged.
QList<Finding> parseFindings(const QString &body, const AuditCheck &check);

// Pure-function counterpart of `AuditDialog::capFindings`. Was
// already static; signature is unchanged.
void capFindings(CheckResult &r, int cap);

// `sourceForCheck` — map a check-id to the canonical "source" tool
// label that gets written to `Finding::source` (and onto SARIF
// `result.runIndex`). One implementation, two callers (engine for
// the parseFindings path, dialog for the per-CheckResult `r.source`
// stamp at populateChecks time). Unifying via the header closes the
// silent-divergence vector ANTS-1123 indie-review C-cluster flagged.
QString sourceForCheck(const QString &checkId);

// Compute a stable 24-hex-char (96-bit) dedup key for a finding.
// Promoted to the public header so a future synthetic-finding site
// (e.g. consolidateMypyStubHints) doesn't have to re-implement it.
QString computeDedup(const QString &file, int line,
                     const QString &checkId, const QString &title);

// Catastrophic-regex shape detector. Rejects patterns whose AST
// shape is known to backtrack pathologically — quantifier inside a
// quantified group `(.+)+`, alternation under a quantifier
// `(a|b)+`, etc. The detector is conservative; it errs on the side
// of rejecting safe-but-suspicious patterns rather than admitting
// adversarial ones. Used as the first line of defense before
// `hardenUserRegex`'s PCRE2 step-limit.
bool isCatastrophicRegex(const QString &pattern);

// Wrap a user-supplied regex with PCRE2's inline `(*LIMIT_MATCH=N)`
// option so a pattern that slips past `isCatastrophicRegex` still
// has a bounded match-step budget. Empty input → empty output;
// patterns already starting with `(*LIMIT_` → returned unchanged
// (avoids double-prefix); else prepends the canonical
// `(*LIMIT_MATCH=100000)` limit. Single source of truth — both the
// engine's `applyFilter` path and the dialog's `audit_rules.json`
// ingest go through this function.
QString hardenUserRegex(const QString &pattern);

}  // namespace AuditEngine
