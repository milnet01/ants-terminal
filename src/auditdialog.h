#pragma once

#include <QDialog>
#include <QTextEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QLabel>
#include <QProcess>
#include <QTimer>
#include <QTemporaryFile>
#include <QRegularExpression>
#include <QSet>

class ToggleSwitch;

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
    // Drop a result line if it contains ANY of these substrings (case-insensitive).
    QStringList dropIfContains;
    // Drop a result line if this regex matches it. Empty = no regex.
    QString dropIfMatches;
    // Keep a result line only if it contains ALL of these substrings (optional).
    QStringList keepOnlyIfContains;
    // Cap output length after filtering. 0 = unlimited.
    int maxLines = 100;
};

struct AuditCheck {
    QString id;
    QString name;
    QString description;
    QString category;
    QString command;           // shell command to run
    CheckType type = CheckType::CodeSmell;
    Severity severity = Severity::Minor;
    OutputFilter filter;
    bool autoSelect = false;
    bool available = true;
    ToggleSwitch *toggle = nullptr;
};

// Individual finding parsed from a check's output. One per issue, not one
// per check — lets us dedup across tools, suppress individually, and render
// severity-sorted even within a check's output.
struct Finding {
    QString checkId;       // originating check
    QString checkName;
    QString category;      // General, Security, Qt, …
    CheckType type;
    Severity severity;
    QString source;        // "cppcheck", "grep", "find", "clang-tidy", …
    QString file;          // "src/terminalgrid.cpp" or empty
    int     line = -1;     // 1-based line number; -1 if not parseable
    QString message;       // the full raw output line (trimmed)
    QString dedupKey;      // SHA-256(file:line:checkId:title) hex, truncated
};

// One finding row after post-processing; kept per-check so we can sort the
// final report by severity and compute summary counts.
struct CheckResult {
    QString checkId;
    QString checkName;
    QString category;
    CheckType type;
    Severity severity;
    QString source;                // primary tool for this check
    QList<Finding> findings;       // parsed individual findings
    int omittedCount = 0;          // from per-check cap — "+ N more"
    QString output;                // raw (post-filter) output for fallback display
    int findingCount = 0;          // convenience: findings.size() + omittedCount
    bool warning = false;          // timeout / start-failed
};

class AuditDialog : public QDialog {
    Q_OBJECT

public:
    explicit AuditDialog(const QString &projectPath, QWidget *parent = nullptr);

signals:
    // Emitted when user clicks "Review with Claude" — carries path to temp results file
    void reviewRequested(const QString &resultsFile);

private:
    // Lifecycle
    void detectProject();
    void populateChecks();
    void buildUI();
    void runAudit();
    void runNextCheck();
    void onCheckFinished(int exitCode, QProcess::ExitStatus status);
    bool toolExists(const QString &tool);

    // Check-definition helpers (collapse repeated boilerplate)
    void addGrepCheck(const QString &id, const QString &name,
                      const QString &desc, const QString &category,
                      const QString &pattern, CheckType type, Severity sev,
                      bool autoSelect, const OutputFilter &filter = {},
                      const QStringList &extraGrepArgs = {});
    void addFindCheck(const QString &id, const QString &name,
                      const QString &desc, const QString &category,
                      const QString &findArgs, CheckType type, Severity sev,
                      bool autoSelect, const OutputFilter &filter = {});
    void addToolCheck(const QString &id, const QString &name,
                      const QString &desc, const QString &category,
                      const QString &tool, const QString &commandTemplate,
                      CheckType type, Severity sev, bool autoSelect,
                      const OutputFilter &filter = {});

    // Apply OutputFilter to raw command output; returns a trimmed, capped body
    // plus a finding count (non-empty line count) for the summary.
    struct FilterResult { QString body; int count; };
    static FilterResult applyFilter(const QString &raw, const OutputFilter &f);

    // Parse command output lines into structured Findings. Understands:
    //   file:line:col: msg           (grep -n, cppcheck, clang-tidy, gcc)
    //   file:line: msg               (shellcheck, some linters)
    //   file                         (find, ls output)
    //   other                        (whole line is a free-form message)
    static QList<Finding> parseFindings(const QString &body,
                                        const AuditCheck &check);

    // Apply per-check cap; overflow entries are dropped and omittedCount
    // is recorded so the UI can show "(+N more)".
    static void capFindings(CheckResult &r, int cap);

    // Load user-maintained dedup keys from <project>/.audit_suppress (one
    // key per line, `#` comments allowed).
    void loadSuppressions();
    QString suppressionPath() const;

    // Present accumulated results sorted by severity with a summary banner.
    void renderResults();

    // Baseline support — store last-run findings as JSON at
    //   <projectPath>/.audit_cache/baseline.json
    // On next run, highlight new findings (not present in baseline).
    void loadBaseline();
    void saveBaseline();
    QString baselinePath() const;

    QString m_projectPath;
    QStringList m_detectedTypes;

    QList<AuditCheck> m_checks;
    QList<CheckResult> m_completedResults;   // accumulated for renderResults()
    int m_currentCheck = -1;
    int m_checksRun = 0;
    int m_totalSelected = 0;

    // Baseline: set of "checkId:first-output-line" fingerprints from last run.
    QSet<QString> m_baselineFingerprints;
    bool m_hasBaseline = false;
    bool m_showNewOnly = false;  // UI toggle

    // Persistent per-project suppressions (by Finding::dedupKey) — read from
    // <project>/.audit_suppress at load time, suppressed findings are hidden
    // from results.
    QSet<QString> m_suppressedKeys;

    // Per-check cap to prevent one noisy check from drowning out signal.
    static constexpr int kMaxFindingsPerCheck = 100;

    QLabel *m_pathLabel = nullptr;
    QLabel *m_typesLabel = nullptr;
    QPushButton *m_runBtn = nullptr;
    QPushButton *m_baselineBtn = nullptr;
    QPushButton *m_newOnlyBtn = nullptr;
    QProgressBar *m_progress = nullptr;
    QTextEdit *m_results = nullptr;
    QLabel *m_statusLabel = nullptr;
    QProcess *m_process = nullptr;
    QTimer *m_timeout = nullptr;

    QPushButton *m_reviewBtn = nullptr;
    QString plainTextResults() const;
};
