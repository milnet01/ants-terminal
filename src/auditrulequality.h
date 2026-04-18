#pragma once

// Self-learning layer for the project Audit dialog (0.6.31).
//
// The audit catalogue carries dozens of grep / find / cppcheck / clazy /
// clang-tidy rules. Rule signal-to-noise drifts over time as the codebase
// evolves and new false-positive shapes appear. Without instrumentation,
// the user is the only feedback channel — they suppress noisy findings
// one at a time, and the rule that produced them keeps producing more
// of the same.
//
// RuleQualityTracker closes that loop. It records:
//
//   - every finding fired by every rule, per audit run, per project
//   - every suppression the user issues (rule, dedup key, matched line,
//     reason, timestamp)
//
// and exposes:
//
//   - per-rule fire / suppression counts over a sliding window
//   - a heuristic tightening suggester (longest common substring across
//     the last N suppressions for a rule) that proposes a candidate
//     `dropIfContains` addition the user can accept with one click
//
// Persisted to `<project>/audit_rule_quality.json` (sibling of
// `.audit_suppress`). JSON not JSONL because the file is read whole on
// every dialog open and the size is small (capped at the 90-day window).
//
// The suggester is INTENTIONALLY simple — a 1-2 line text heuristic, not
// an ML model. Two reasons: (a) it has to run synchronously inside the
// suppression handler so the prompt feels immediate, and (b) the user
// always vets the suggestion before it's applied to `audit_rules.json`.
// More expensive analysis (LLM-driven rule-tightening proposals across
// the whole catalogue) lives in the weekly cloud routine documented in
// `docs/RECOMMENDED_ROUTINES.md` §3.

#include <QString>
#include <QStringList>
#include <QDateTime>
#include <QHash>
#include <QVector>

class RuleQualityTracker {
public:
    explicit RuleQualityTracker(const QString &projectPath);

    // Record a single finding firing for `ruleId`. Called once per
    // finding from the audit-dialog post-processing path. `lineText` is
    // the matched line (file:line:col: msg shape, or the raw output line
    // for tools that don't carry a file location). Empty `lineText` is
    // tolerated — the fire is still counted, but the LCS suggester can't
    // use it.
    void recordFire(const QString &ruleId, const QString &lineText);

    // Record a user suppression of a single finding. Called from
    // AuditDialog::saveSuppression after the suppression is persisted to
    // .audit_suppress. The same arguments + the matched line text and
    // user reason are stored here so the suggester has source material
    // for finding common shapes.
    void recordSuppression(const QString &ruleId,
                           const QString &dedupKey,
                           const QString &lineText,
                           const QString &reason);

    // Per-rule report row.
    struct RuleStats {
        QString ruleId;
        int fires30d = 0;          // finding fires in the last 30 days
        int suppressions30d = 0;   // suppressions in the last 30 days
        int firesAllTime = 0;
        int suppressionsAllTime = 0;
        QDateTime lastFire;
        QDateTime lastSuppression;
        // suppression / fires30d expressed as a percentage. Capped at
        // 100; -1 means "no fires" (avoid spurious 0 / NaN entries).
        int fpRate30d = -1;
    };
    QVector<RuleStats> report() const;

    // Heuristic tightening suggester. Looks at the last `maxSamples`
    // suppressed lines for `ruleId` and returns a candidate substring
    // that, if added to the rule's `dropIfContains`, would have
    // suppressed all of them. Empty string = no clean common substring
    // found (the suppressions are too varied for this trivial heuristic
    // to help — fall through to the cloud-routine analysis).
    //
    // Algorithm: longest common substring across the line texts, with a
    // minimum length floor (default 6 chars) to avoid suggesting a
    // single common token like `if` or `()`. Whitespace-trimmed; we
    // only suggest substrings that contain at least one non-alphanumeric
    // boundary character (space, paren, brace, semicolon) so the
    // candidate has SOME structural meaning rather than being a
    // random identifier fragment.
    QString suggestTightening(const QString &ruleId,
                              int maxSamples = 5,
                              int minLength = 6) const;

    // Force-flush to disk. Called automatically on destruction; exposed
    // for the tests + the AuditDialog "Rule Quality" view-open path.
    void save() const;

    // Reload from disk — used by the unit tests, not production.
    void reload();

    ~RuleQualityTracker() { save(); }

private:
    struct FireRecord {
        QString ruleId;
        QString lineText;
        QDateTime timestamp;
    };
    struct SuppressRecord {
        QString ruleId;
        QString dedupKey;
        QString lineText;
        QString reason;
        QDateTime timestamp;
    };

    QString m_projectPath;
    QString m_path;
    QVector<FireRecord> m_fires;
    QVector<SuppressRecord> m_suppressions;

    // Bounded retention. We keep at most this many records per category
    // — older ones are pruned on save. 90-day equivalent at typical run
    // frequency, with headroom for pathological days.
    static constexpr int MAX_RECORDS = 50000;
    static constexpr int RETENTION_DAYS = 90;

    void load();
};
