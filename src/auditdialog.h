#pragma once

#include "elidedlabel.h"

#include <QDialog>
#include <QTextBrowser>
#include <QPushButton>
#include <QProgressBar>
#include <QLabel>
#include <QProcess>
#include <QTimer>
#include <QTemporaryFile>
#include <QRegularExpression>
#include <QSet>
#include <QHash>

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
    bool    highConfidence = false; // true when ≥2 distinct tools flag the same line

    // Context-aware enrichment — populated during renderResults() so all
    // exports (UI, HTML, SARIF, plain-text) see the same data.
    int     confidence = -1;   // 0-100 weighted score; -1 = not computed yet
    QString snippet;           // ±N lines around the finding (newline-joined)
    int     snippetStart = 0;  // first line number of the snippet
    // git-blame bag — empty when not a git repo or blame failed.
    QString blameAuthor;       // e.g. "Anthony Schemel"
    QString blameDate;         // ISO date of author time ("2026-04-11")
    QString blameSha;          // 8-char short SHA
    // AI-triage cache — populated when user clicks "🧠 Triage" on this finding.
    QString aiVerdict;         // "TRUE_POSITIVE" | "FALSE_POSITIVE" | "NEEDS_REVIEW"
    int     aiConfidence = -1; // 0-100 from the model, -1 = not triaged
    QString aiReasoning;       // model's explanation (truncated to ~600 chars)
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

    // Load user-defined rules from <project>/audit_rules.json. Schema:
    //   { "version": 1,
    //     "rules": [ { id, name, description, category,
    //       severity: info|minor|major|critical|blocker,
    //       type:     info|smell|bug|hotspot|vuln,
    //       command, auto_select, max_lines,
    //       drop_if_contains: [...], keep_only_if_contains: [...],
    //       drop_if_matches: "regex" } ],
    //     "path_rules": [ { glob, skip?, severity_shift?, skip_rules?[] } ]
    //   }
    // User rules are appended to m_checks after hardcoded checks and inherit
    // the same filter / parse / suppress / baseline machinery. Path rules
    // apply during the parse pipeline regardless of which check produced the
    // finding — see applyPathRules().
    int  loadUserRules();
    QString userRulesPath() const;

    // Per-project path rule — loaded from audit_rules.json. Three effects:
    //   skip=true          — drop the finding entirely
    //   severity_shift=-N  — shift severity down N levels (+N shifts up)
    //   skip_rules=[...]   — drop finding if its checkId is in the list
    // Evaluated in declaration order; first matching rule wins per axis.
    struct PathRule {
        QString glob;                 // e.g. "tests/**", "**/moc_*.cpp"
        QRegularExpression compiled;  // pre-compiled from glob
        bool    skip = false;
        int     severityShift = 0;
        QStringList skipRules;
    };
    QList<PathRule> m_pathRules;
    // Apply path rules + generated-file skip to a single finding; returns
    // false if the finding should be dropped entirely.
    bool applyPathRules(Finding &f) const;
    static bool isGeneratedFile(const QString &path);
    // Compile a glob string (supporting ** and *) into a QRegularExpression.
    static QRegularExpression globToRegex(const QString &glob);

    // Load user-maintained dedup keys from <project>/.audit_suppress.
    // Format evolution:
    //   v1 (legacy): one dedup-key per line, '#' comments allowed.
    //   v2 (current): one JSON object per line — JSONL with fields
    //     {"key", "rule", "reason", "timestamp"}. Loader auto-detects.
    void loadSuppressions();
    QString suppressionPath() const;

    // Append a suppression entry — always writes JSONL v2. If the file is
    // still in v1 format it's converted in-place on first write.
    void saveSuppression(const QString &dedupKey,
                         const QString &ruleId,
                         const QString &reason);

    // Slot wired to QTextBrowser::anchorClicked in the results pane —
    // interprets ants-suppress://<key> links, shows the reason prompt, and
    // persists via saveSuppression().
    void onResultAnchorClicked(const QUrl &url);

    // Trend tracking — save a severity-count snapshot after each run, read
    // the prior one to compute delta (shown in the summary banner).
    struct TrendSnapshot {
        QString timestamp;   // ISO 8601
        int total = 0;
        int bySev[5] = {0, 0, 0, 0, 0};
    };
    QString trendPath() const;
    TrendSnapshot loadLastSnapshot() const;
    void appendSnapshot(const TrendSnapshot &s);
    static constexpr int kMaxTrendHistory = 50;

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

    // Filesystem type of the project root (e.g. "ext2/ext3", "btrfs", "fuseblk",
    // "ntfs", "vfat"). Populated once at construction via `stat -f -c %T`.
    // Used to suppress POSIX-only checks (e.g. world-writable permissions)
    // when the project lives on a filesystem that doesn't enforce them — on
    // such mounts, every file appears world-writable regardless of intent.
    // Empty string if detection failed; treated as POSIX in that case.
    QString m_projectFsType;
    bool isPosixFilesystem() const;  // returns false for fuseblk/ntfs/vfat/…

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

    // Lookup table populated at render time so the anchor click handler can
    // find a finding's context (file, message, rule) from just the dedup key.
    QHash<QString, Finding> m_findingsByKey;

    // Per-check cap to prevent one noisy check from drowning out signal.
    static constexpr int kMaxFindingsPerCheck = 100;

    // "Recent changes only" mode — scope each check to files touched in the
    // last N commits. Zero/disabled = whole-project audit.
    // When m_recentLinesOnly is also on, findings are further filtered to the
    // exact line ranges from `git diff -U0` — catches what CI would flag in
    // a PR review without the noise of pre-existing issues elsewhere in the
    // changed file.
    bool m_recentOnly = false;
    bool m_recentLinesOnly = false;
    int  m_recentCommits = 10;
    QStringList m_recentFiles;                 // computed at runAudit() start
    QHash<QString, QSet<int>> m_recentLines;   // file → {modified line numbers}

    // Project context docs to attach to the Claude-review handoff. Loaded
    // lazily when "Review with Claude" is pressed.
    QString readProjectDoc(const QString &name) const;

    // Filter findings whose source location lives inside a comment or string
    // literal (per-file state-machine scan). Reference-tool-proven #1 source
    // of pattern-match false positives — e.g. "new" inside "// allocate with
    // new" used to flag every audit.
    void dropFindingsInCommentsOrStrings(CheckResult &r) const;
    // Classify the 1-based line of a file as code / comment-only / string-
    // literal-only. Returns true if the line is safe to keep (code or
    // unknown file), false if it's confined to comments/strings.
    static bool lineIsCode(const QString &absPath, int line);

    // Inline suppression directive scan. Recognises both ants-native tokens
    // and passthrough markers from other tools (clang-tidy NOLINT, cppcheck
    // suppress-comments, flake8 noqa, bandit nosec, semgrep nosemgrep,
    // gitleaks-allow, eslint-disable-*, pylint: disable). Returns true iff
    // the finding's line is explicitly suppressed for its rule id.
    // (Doc deliberately avoids putting the literal "cppcheck-" + "suppress"
    // token at the start of a comment line — cppcheck's --inline-suppr
    // parser would otherwise pick it up and emit invalidSuppression.)
    bool inlineSuppressed(const Finding &f) const;
    // Low-level helper: does `commentText` (already-stripped "// ..." body)
    // contain a suppression token that matches `ruleId`? Returns true if:
    //   - bare suppress (no rule list)     → always suppress
    //   - rule-list contains ruleId        → suppress
    //   - glob 'rule-*' matches ruleId     → suppress
    static bool commentSuppresses(const QString &commentText, const QString &ruleId);

    // Read ±radius lines around `line` from `absPath`. Returns the extracted
    // text (newline-joined) and the starting line number via out-params.
    // Used for snippet context and AI-triage payloads.
    QString readSnippet(const QString &absPath, int line, int radius,
                        int *startLineOut = nullptr) const;
    // Per-file cache — a single audit run may touch dozens of findings in the
    // same file; reading once is much cheaper than per-finding disk I/O.
    mutable QHash<QString, QStringList> m_fileLineCache;

    // Git-blame enrichment — shells out to `git blame --porcelain -L N,N`,
    // caches by (relPath:line) so multiple findings on the same line are
    // free. Populates Finding::blameAuthor/Date/Sha in place.
    void enrichWithBlame(Finding &f) const;
    struct BlameEntry { QString author; QString date; QString sha; };
    mutable QHash<QString, BlameEntry> m_blameCache;
    bool m_blameEnabled = true;   // auto-disabled if not a git repo

    // Confidence score (0-100) computed from severity, multi-tool agreement,
    // tool class, path heuristics. Populated during renderResults() after
    // correlation. Formula is documented inline at the definition.
    static int computeConfidence(const Finding &f);

    // Fire-and-forget AI triage of a single finding. POSTs the rule +
    // snippet + blame to the project's configured OpenAI-compatible chat
    // endpoint; updates Finding::aiVerdict/aiConfidence/aiReasoning on the
    // next render. Requires Config::aiEnabled + ai_endpoint + ai_api_key.
    void requestAiTriage(const QString &dedupKey);

    // SARIF v2.1.0 export — OASIS-standard JSON format consumed by GitHub
    // code-scanning, VSCode SARIF Viewer, SonarQube, etc.
    QString exportSarif() const;

    // Single-file HTML report — self-contained (no external CDN or JS deps)
    // with severity pills, text filter, and per-check collapsible cards.
    // Useful for attaching to bug reports or sharing with teammates.
    QString exportHtml() const;

    QLabel *m_pathLabel = nullptr;
    QLabel *m_typesLabel = nullptr;
    QPushButton *m_runBtn = nullptr;
    QPushButton *m_baselineBtn = nullptr;
    QPushButton *m_newOnlyBtn = nullptr;
    QProgressBar *m_progress = nullptr;
    QTextBrowser *m_results = nullptr;
    // "Running: <check-name>…", "SARIF saved: <path>", etc. — long paths /
    // long check names would push downstream widgets off. ElidedLabel keeps
    // the dialog footer stable-width regardless of message length.
    ElidedLabel *m_statusLabel = nullptr;
    QProcess *m_process = nullptr;
    QTimer *m_timeout = nullptr;

    QPushButton *m_reviewBtn = nullptr;
    QPushButton *m_sarifBtn = nullptr;
    QPushButton *m_htmlBtn = nullptr;
    QString plainTextResults() const;

    // Filter bar — shown once results exist. Severity pill toggles, a live
    // text filter, and a "sort by confidence" toggle. All live-re-render.
    class QLineEdit   *m_filterInput = nullptr;
    QList<QPushButton*> m_sevPills;        // one per Severity
    QPushButton        *m_confidenceSortBtn = nullptr;
    QWidget            *m_filterBar = nullptr;
    QString             m_textFilter;                          // lowercased
    QSet<int>           m_activeSeverities = {0, 1, 2, 3, 4};  // all on by default
    bool                m_sortByConfidence = false;
    // Per-finding expand state for the collapsible snippet/blame/triage
    // section. Keyed by dedupKey so toggles survive re-renders.
    QSet<QString>       m_expandedKeys;
};
