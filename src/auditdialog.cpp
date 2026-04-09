#include "auditdialog.h"
#include "toggleswitch.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QScrollArea>
#include <QDir>
#include <QFileInfo>
#include <QDirIterator>
#include <QFont>
#include <QScrollBar>
#include <QApplication>

AuditDialog::AuditDialog(const QString &projectPath, QWidget *parent)
    : QDialog(parent), m_projectPath(projectPath) {
    setWindowTitle("Project Audit");
    setMinimumSize(750, 600);
    resize(850, 700);

    m_process = new QProcess(this);
    m_process->setWorkingDirectory(m_projectPath);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &AuditDialog::onCheckFinished);

    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, [this]() {
        if (m_process->state() != QProcess::NotRunning) {
            // Disconnect finished signal to prevent double-advance after kill
            disconnect(m_process, nullptr, this, nullptr);
            m_process->kill();
            m_process->waitForFinished(1000);
            // Reconnect for next check
            connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                    this, &AuditDialog::onCheckFinished);
            if (m_currentCheck >= 0 && m_currentCheck < m_checks.size())
                appendResult(m_checks[m_currentCheck].name, "Timed out (30s)", true);
            ++m_checksRun;
            runNextCheck();
        }
    });

    detectProject();
    populateChecks();
    buildUI();
}

// ---------------------------------------------------------------------------
// Project detection
// ---------------------------------------------------------------------------

void AuditDialog::detectProject() {
    QDir dir(m_projectPath);

    if (dir.exists(".git"))
        m_detectedTypes << "Git";
    if (dir.exists("CMakeLists.txt") || dir.exists("Makefile"))
        m_detectedTypes << "C/C++";
    if (dir.exists("package.json"))
        m_detectedTypes << "JavaScript";
    if (dir.exists("Cargo.toml"))
        m_detectedTypes << "Rust";
    if (dir.exists("go.mod"))
        m_detectedTypes << "Go";

    // Scan top-level + one level deep for language indicators
    QDirIterator it(m_projectPath, QDir::Files, QDirIterator::NoIteratorFlags);
    bool hasPy = false, hasSh = false, hasLua = false, hasJava = false;
    bool hasCpp = false;
    int scanned = 0;
    while (it.hasNext() && scanned < 200) {
        it.next();
        ++scanned;
        QString suf = it.fileInfo().suffix().toLower();
        if (suf == "py" || suf == "pyw") hasPy = true;
        if (suf == "sh" || suf == "bash") hasSh = true;
        if (suf == "lua") hasLua = true;
        if (suf == "java") hasJava = true;
        if (suf == "cpp" || suf == "c" || suf == "cc" || suf == "cxx") hasCpp = true;
    }
    // Also check src/ subdirectory
    QDirIterator srcIt(m_projectPath + "/src", QDir::Files, QDirIterator::NoIteratorFlags);
    scanned = 0;
    while (srcIt.hasNext() && scanned < 200) {
        srcIt.next();
        ++scanned;
        QString suf = srcIt.fileInfo().suffix().toLower();
        if (suf == "py" || suf == "pyw") hasPy = true;
        if (suf == "sh" || suf == "bash") hasSh = true;
        if (suf == "lua") hasLua = true;
        if (suf == "java") hasJava = true;
        if (suf == "cpp" || suf == "c" || suf == "cc" || suf == "cxx") hasCpp = true;
    }

    if (hasCpp && !m_detectedTypes.contains("C/C++"))
        m_detectedTypes << "C/C++";
    if (hasPy) m_detectedTypes << "Python";
    if (hasSh) m_detectedTypes << "Shell";
    if (hasLua) m_detectedTypes << "Lua";
    if (hasJava && !m_detectedTypes.contains("Java"))
        m_detectedTypes << "Java";
}

// ---------------------------------------------------------------------------
// Check definitions
// ---------------------------------------------------------------------------

bool AuditDialog::toolExists(const QString &tool) {
    QProcess p;
    p.start("which", {tool});
    p.waitForFinished(3000);
    return p.exitCode() == 0;
}

void AuditDialog::populateChecks() {
    // Exclude patterns common to all find/grep commands
    const QString X = " -not -path './.git/*' -not -path './build/*'"
                      " -not -path './node_modules/*' -not -path './.cache/*'";
    const QString GI = " --include='*.cpp' --include='*.h' --include='*.c'"
                       " --include='*.py' --include='*.js' --include='*.ts'"
                       " --include='*.go' --include='*.rs' --include='*.sh'"
                       " --include='*.lua' --include='*.java'";

    // ---- Generic (always available) ----
    m_checks.append({
        "todo_scan", "TODO / FIXME Scanner",
        "Find TODO, FIXME, HACK, XXX annotations", "General",
        "grep -rnI" + GI + " -E '(TODO|FIXME|HACK|XXX)(\\(|:|\\s)' . 2>/dev/null | head -100",
        true, true, {}
    });
    m_checks.append({
        "large_files", "Large File Finder",
        "Files larger than 500 KB", "General",
        "find ." + X + " -type f -size +500k -exec ls -lh {} \\; 2>/dev/null"
        " | awk '{print $5, $9}' | sort -rh | head -30",
        true, true, {}
    });
    m_checks.append({
        "line_stats", "Line Count Statistics",
        "Lines of code by file (top 25)", "General",
        "find ." + X + " -type f \\( -name '*.cpp' -o -name '*.h' -o -name '*.c'"
        " -o -name '*.py' -o -name '*.js' -o -name '*.ts' -o -name '*.go'"
        " -o -name '*.rs' -o -name '*.sh' -o -name '*.lua' -o -name '*.java' \\)"
        " | xargs wc -l 2>/dev/null | sort -rn | head -25",
        false, true, {}
    });
    m_checks.append({
        "secrets_scan", "Hardcoded Secrets Scan",
        "Grep for API keys, passwords, tokens in source", "Security",
        "grep -rnI" + GI + " --include='*.json' --include='*.yaml' --include='*.yml'"
        " --include='*.toml' --include='*.cfg' --include='*.ini'"
        " -iE '(api[_-]?key|password|secret[_-]?key|auth[_-]?token|credentials)\\s*[:=]' . 2>/dev/null"
        " | grep -viE '(test|example|mock|dummy|placeholder|TODO|FIXME|template|sample)' | head -50",
        true, true, {}
    });
    m_checks.append({
        "file_perms", "File Permission Check",
        "World-writable or group-writable files", "Security",
        "find ." + X + " -type f \\( -perm -002 -o -perm -020 \\) 2>/dev/null | head -30",
        false, true, {}
    });
    m_checks.append({
        "readme_check", "README & License Check",
        "Verify documentation files exist", "General",
        "echo '=== README ===' && (ls README* readme* 2>/dev/null || echo 'No README found')"
        " && echo '=== LICENSE ===' && (ls LICENSE* license* COPYING* 2>/dev/null || echo 'No LICENSE found')",
        false, true, {}
    });
    m_checks.append({
        "dup_files", "Duplicate File Detection",
        "Find files with identical content", "General",
        "find ." + X + " -type f -size +100c \\( -name '*.cpp' -o -name '*.h' -o -name '*.py'"
        " -o -name '*.js' -o -name '*.ts' \\) -exec md5sum {} + 2>/dev/null"
        " | sort | uniq -Dw32 | head -30",
        false, true, {}
    });

    // ---- Git ----
    if (m_detectedTypes.contains("Git")) {
        m_checks.append({
            "git_status", "Uncommitted Changes",
            "Show working tree status", "Git",
            "git status --short 2>/dev/null",
            true, true, {}
        });
        m_checks.append({
            "git_stale", "Branch Overview",
            "Merged and unmerged branches", "Git",
            "echo '=== Unmerged ===' && git branch -v --no-merged 2>/dev/null"
            " && echo '=== Merged (can delete) ===' && git branch -v --merged 2>/dev/null | grep -v '^\\*'",
            false, true, {}
        });
    }

    // ---- C/C++ ----
    if (m_detectedTypes.contains("C/C++")) {
        bool hasCppcheck = toolExists("cppcheck");
        m_checks.append({
            "cppcheck", "cppcheck Static Analysis",
            hasCppcheck ? "Warnings, performance, portability" : "(cppcheck not installed)",
            "C/C++",
            "cppcheck --enable=warning,performance,portability --quiet --inline-suppr"
            " --suppress=missingInclude --suppress=unmatchedSuppression -j$(nproc) . 2>&1 | head -100",
            hasCppcheck, hasCppcheck, {}
        });
        bool hasClangTidy = toolExists("clang-tidy");
        m_checks.append({
            "clang_tidy", "clang-tidy Analysis",
            hasClangTidy ? "Modernize, readability, performance checks" : "(clang-tidy not installed)",
            "C/C++",
            "find . -name '*.cpp'" + X + " | head -15"
            " | xargs -I{} clang-tidy {} -- -std=c++20 -I src 2>&1 | head -100",
            false, hasClangTidy, {}
        });
    }

    // ---- Python ----
    if (m_detectedTypes.contains("Python")) {
        bool hasPylint = toolExists("pylint");
        m_checks.append({
            "pylint", "Pylint Analysis",
            hasPylint ? "Error-level checks" : "(pylint not installed)",
            "Python",
            "find . -name '*.py'" + X + " | head -20 | xargs pylint --errors-only 2>&1 | head -100",
            hasPylint, hasPylint, {}
        });
        bool hasBandit = toolExists("bandit");
        m_checks.append({
            "bandit", "Bandit Security Scan",
            hasBandit ? "Python security issue detection" : "(bandit not installed)",
            "Python",
            "bandit -r . -q 2>&1 | head -100",
            hasBandit, hasBandit, {}
        });
        bool hasMypy = toolExists("mypy");
        m_checks.append({
            "mypy", "mypy Type Check",
            hasMypy ? "Static type analysis" : "(mypy not installed)",
            "Python",
            "mypy . --ignore-missing-imports 2>&1 | tail -20",
            false, hasMypy, {}
        });
    }

    // ---- JavaScript / TypeScript ----
    if (m_detectedTypes.contains("JavaScript")) {
        bool hasNpm = toolExists("npm");
        m_checks.append({
            "npm_audit", "npm Dependency Audit",
            hasNpm ? "Check for known vulnerabilities" : "(npm not installed)",
            "JavaScript",
            "npm audit --production 2>&1 | tail -30",
            hasNpm, hasNpm, {}
        });
        m_checks.append({
            "eslint", "ESLint Analysis",
            hasNpm ? "Lint JavaScript/TypeScript" : "(npm not installed)",
            "JavaScript",
            "npx eslint . --max-warnings=50 2>&1 | head -100",
            false, hasNpm, {}
        });
    }

    // ---- Rust ----
    if (m_detectedTypes.contains("Rust")) {
        bool hasCargo = toolExists("cargo");
        m_checks.append({
            "cargo_clippy", "Cargo Clippy",
            hasCargo ? "Rust lint checks" : "(cargo not installed)",
            "Rust",
            "cargo clippy 2>&1 | head -100",
            hasCargo, hasCargo, {}
        });
        bool hasAudit = toolExists("cargo-audit");
        m_checks.append({
            "cargo_audit", "Cargo Audit",
            hasAudit ? "Dependency vulnerability scan" : "(cargo-audit not installed)",
            "Rust",
            "cargo audit 2>&1 | head -100",
            hasAudit, hasAudit, {}
        });
    }

    // ---- Go ----
    if (m_detectedTypes.contains("Go")) {
        bool hasGo = toolExists("go");
        m_checks.append({
            "go_vet", "Go Vet",
            hasGo ? "Report likely mistakes" : "(go not installed)",
            "Go",
            "go vet ./... 2>&1 | head -100",
            hasGo, hasGo, {}
        });
        bool hasVuln = toolExists("govulncheck");
        m_checks.append({
            "govulncheck", "Go Vulnerability Check",
            hasVuln ? "Scan dependencies for known vulnerabilities" : "(govulncheck not installed)",
            "Go",
            "govulncheck ./... 2>&1 | head -50",
            false, hasVuln, {}
        });
    }

    // ---- Shell ----
    if (m_detectedTypes.contains("Shell")) {
        bool hasSC = toolExists("shellcheck");
        m_checks.append({
            "shellcheck", "ShellCheck Analysis",
            hasSC ? "Static analysis for shell scripts" : "(shellcheck not installed)",
            "Shell",
            "find . -name '*.sh'" + X + " -exec shellcheck {} + 2>&1 | head -100",
            hasSC, hasSC, {}
        });
    }
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void AuditDialog::buildUI() {
    auto *root = new QVBoxLayout(this);
    root->setSpacing(8);

    // Header
    m_pathLabel = new QLabel(this);
    m_pathLabel->setText("<b>Project:</b> " + m_projectPath);
    m_pathLabel->setTextFormat(Qt::RichText);
    root->addWidget(m_pathLabel);

    m_typesLabel = new QLabel(this);
    QString types = m_detectedTypes.isEmpty() ? "Unknown" : m_detectedTypes.join(", ");
    m_typesLabel->setText("<b>Detected:</b> " + types);
    m_typesLabel->setTextFormat(Qt::RichText);
    root->addWidget(m_typesLabel);

    root->addSpacing(4);

    // Scrollable check list
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *scrollWidget = new QWidget();
    auto *scrollLayout = new QVBoxLayout(scrollWidget);
    scrollLayout->setSpacing(6);
    scrollLayout->setContentsMargins(0, 0, 8, 0);

    // Group checks by category
    QStringList categories;
    for (const auto &c : m_checks) {
        if (!categories.contains(c.category))
            categories << c.category;
    }

    for (const QString &cat : categories) {
        auto *group = new QGroupBox(cat, scrollWidget);
        auto *gLayout = new QVBoxLayout(group);
        gLayout->setSpacing(4);
        gLayout->setContentsMargins(10, 14, 10, 8);

        for (int i = 0; i < m_checks.size(); ++i) {
            auto &check = m_checks[i];
            if (check.category != cat) continue;

            auto *row = new QHBoxLayout();
            row->setSpacing(8);

            auto *nameLabel = new QLabel(check.name, group);
            QFont f = nameLabel->font();
            f.setWeight(QFont::Medium);
            nameLabel->setFont(f);
            row->addWidget(nameLabel);

            auto *descLabel = new QLabel(check.description, group);
            descLabel->setStyleSheet("color: #888;");
            row->addWidget(descLabel);
            row->addStretch();

            auto *toggle = new ToggleSwitch(group);
            toggle->setChecked(check.autoSelect && check.available);
            toggle->setEnabled(check.available);
            check.toggle = toggle;
            row->addWidget(toggle);

            gLayout->addLayout(row);
        }

        scrollLayout->addWidget(group);
    }
    scrollLayout->addStretch();
    scroll->setWidget(scrollWidget);
    root->addWidget(scroll, 1);

    // Button row
    auto *btnRow = new QHBoxLayout();
    auto *allOn = new QPushButton("All On", this);
    auto *allOff = new QPushButton("All Off", this);
    allOn->setFixedWidth(70);
    allOff->setFixedWidth(70);
    connect(allOn, &QPushButton::clicked, this, [this]() {
        for (auto &c : m_checks)
            if (c.toggle && c.available) c.toggle->setChecked(true);
    });
    connect(allOff, &QPushButton::clicked, this, [this]() {
        for (auto &c : m_checks)
            if (c.toggle) c.toggle->setChecked(false);
    });
    btnRow->addWidget(allOn);
    btnRow->addWidget(allOff);
    btnRow->addStretch();

    m_runBtn = new QPushButton("Run Audit", this);
    m_runBtn->setFixedHeight(32);
    m_runBtn->setMinimumWidth(120);
    connect(m_runBtn, &QPushButton::clicked, this, &AuditDialog::runAudit);
    btnRow->addWidget(m_runBtn);
    root->addLayout(btnRow);

    // Progress
    m_progress = new QProgressBar(this);
    m_progress->setTextVisible(true);
    m_progress->setVisible(false);
    root->addWidget(m_progress);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setVisible(false);
    root->addWidget(m_statusLabel);

    // Results
    m_results = new QTextEdit(this);
    m_results->setReadOnly(true);
    m_results->setFont(QFont("monospace", 9));
    m_results->setVisible(false);
    root->addWidget(m_results, 2);
}

// ---------------------------------------------------------------------------
// Audit execution
// ---------------------------------------------------------------------------

void AuditDialog::runAudit() {
    m_results->clear();
    m_results->setVisible(true);
    m_progress->setVisible(true);
    m_statusLabel->setVisible(true);

    m_totalSelected = 0;
    for (const auto &c : m_checks)
        if (c.toggle && c.toggle->isChecked()) ++m_totalSelected;

    if (m_totalSelected == 0) {
        m_statusLabel->setText("No checks selected.");
        m_progress->setVisible(false);
        return;
    }

    m_progress->setRange(0, m_totalSelected);
    m_progress->setValue(0);
    m_checksRun = 0;
    m_currentCheck = -1;
    m_runBtn->setEnabled(false);

    // Disable toggles during run
    for (auto &c : m_checks)
        if (c.toggle) c.toggle->setEnabled(false);

    runNextCheck();
}

void AuditDialog::runNextCheck() {
    // Find next selected check
    while (++m_currentCheck < m_checks.size()) {
        if (m_checks[m_currentCheck].toggle &&
            m_checks[m_currentCheck].toggle->isChecked())
            break;
    }

    if (m_currentCheck >= m_checks.size()) {
        // All done
        m_progress->setValue(m_totalSelected);
        m_statusLabel->setText(QString("Audit complete - %1 checks run").arg(m_totalSelected));
        m_runBtn->setEnabled(true);
        // Re-enable toggles
        for (auto &c : m_checks)
            if (c.toggle) c.toggle->setEnabled(c.available);
        return;
    }

    const auto &check = m_checks[m_currentCheck];
    m_statusLabel->setText("Running: " + check.name + "...");
    m_progress->setValue(m_checksRun);

    m_timeout->start(30000);
    m_process->start("/bin/bash", {"-c", check.command});
    if (!m_process->waitForStarted(5000)) {
        m_timeout->stop();
        appendResult(check.name, "Failed to start process", true);
        ++m_checksRun;
        runNextCheck();
    }
}

void AuditDialog::onCheckFinished(int /*exitCode*/, QProcess::ExitStatus /*status*/) {
    m_timeout->stop();
    if (m_currentCheck < 0 || m_currentCheck >= m_checks.size()) return;

    const auto &check = m_checks[m_currentCheck];
    QString output = QString::fromUtf8(m_process->readAllStandardOutput()).trimmed();
    QString errOutput = QString::fromUtf8(m_process->readAllStandardError()).trimmed();

    if (!errOutput.isEmpty() && output.isEmpty())
        output = errOutput;
    else if (!errOutput.isEmpty())
        output += "\n" + errOutput;

    if (output.isEmpty())
        output = "No issues found.";

    appendResult(check.name, output);
    ++m_checksRun;
    runNextCheck();
}

void AuditDialog::appendResult(const QString &title, const QString &output, bool isWarning) {
    QString color = isWarning ? "#e74856" : "#4CAF50";
    m_results->append(QString(
        "<div style='margin-bottom:8px;'>"
        "<span style='color:%1; font-weight:bold; font-size:11px;'>--- %2 ---</span><br>"
        "<pre style='margin:2px 0; white-space:pre-wrap;'>%3</pre>"
        "</div>"
    ).arg(color, title.toHtmlEscaped(), output.toHtmlEscaped()));

    // Auto-scroll to bottom
    auto *sb = m_results->verticalScrollBar();
    if (sb) sb->setValue(sb->maximum());
}
