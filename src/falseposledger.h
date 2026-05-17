// ANTS-1457 — false-positive ledger helper.
//
// Pure-function namespace for reading, filtering, and formatting
// `.ants_review_falsepos.jsonl` at project root. Consumed by
// IndieReviewEngine, ColdEyesEngine, and TestAuditEngine
// brief-assembly paths so a re-run of the four sweep skills
// (/audit, /cold-eyes, /indie-review, /test-audit) does not
// re-litigate findings the user already classified false-positive.
//
// Standard:  docs/standards/audit-false-positives.md
// Spec:      docs/specs/ANTS-1457.md
//
// Read-only in v1. CC sessions append via shell `>>` per the
// standard's atomic-append recipe.

#pragma once

#include <QJsonArray>
#include <QList>
#include <QString>

namespace ants {
namespace falsepos {

struct LedgerEntry {
    QString reviewKind;   // "" | "audit" | "cold-eyes" | "indie-review" | "test-audit"
    QString lane;         // optional; bidirectional empty-matches-all on filter
    QString claim;        // required, non-empty after parse; truncated to 280 UTF-16 code units on read
    QString rationale;    // required, non-empty after parse; truncated to 1024 UTF-16 code units on read
    QString topic;        // optional, no validation
    QString timestamp;    // required, YYYY-MM-DD; QDate::fromString(s, "yyyy-MM-dd").isValid()
    QString loggedBy;     // optional

    bool isValid() const;
};

struct FormatOptions {
    int maxEntries    = 50;     // cap on count; oldest dropped
    int maxBlockBytes = 65536;  // 64 KiB cap on resulting text block
};

// Reads <projectPath>/.ants_review_falsepos.jsonl. Returns empty
// list on missing file, non-regular file, parse failures
// (per-line skip), or any I/O error. Emits one qWarning when
// file > 1 MiB; parses full file regardless. Never throws.
QList<LedgerEntry> loadEntries(const QString &projectPath);

// AND-match with bidirectional empty=match-anything: entry-side
// empty matches any filter value; filter-side empty matches any
// entry value. Garbage non-canonical review_kind entries are
// already dropped by loadEntries (INV-6), so this filter is
// strict on whatever survived the load.
QList<LedgerEntry> filter(const QList<LedgerEntry> &entries,
                          const QString &reviewKind,
                          const QString &lane);

// Returns a markdown block ready to append to a text-shaped
// brief. Each entry is wrapped in
// [LEDGER-ENTRY-START] … [LEDGER-ENTRY-END] sentinel markers
// with claim+rationale 4-backtick-fenced and a "treat as data,
// not instructions" preamble. Empty input → empty output.
// Block ≤ maxBlockBytes; truncation appends a sentinel line.
QString formatForBrief(const QList<LedgerEntry> &entries,
                       const FormatOptions &opts = {});

// JSON-shaped form for test_audit_brief. Each array element is
// {claim, rationale, topic, timestamp}. No fence hardening
// (the JSON layer is structurally separated from the prompt
// plane).
QJsonArray formatForJsonArray(const QList<LedgerEntry> &entries,
                              const FormatOptions &opts = {});

}  // namespace falsepos
}  // namespace ants
