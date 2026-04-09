#include "claudeprojects.h"
#include "claudeintegration.h"
#include "config.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QSplitter>
#include <QScrollBar>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFileDialog>
#include <QInputDialog>
#include <QFont>
#include <QMessageBox>

ClaudeProjectsDialog::ClaudeProjectsDialog(ClaudeIntegration *integration,
                                             Config *config,
                                             QWidget *parent)
    : QDialog(parent), m_integration(integration), m_config(config) {
    setWindowTitle("Claude Code - Projects & Sessions");
    setMinimumSize(900, 600);
    resize(1050, 700);

    auto *mainLayout = new QVBoxLayout(this);

    // -- Top bar: Project directory management --
    auto *dirRow = new QHBoxLayout();
    dirRow->addWidget(new QLabel("Project directories:", this));

    m_projectDirCombo = new QComboBox(this);
    m_projectDirCombo->setMinimumWidth(300);
    m_projectDirCombo->setToolTip("Directories where you keep your projects");
    dirRow->addWidget(m_projectDirCombo, 1);

    auto *addDirBtn = new QPushButton("+", this);
    addDirBtn->setFixedWidth(30);
    addDirBtn->setToolTip("Add a project directory");
    connect(addDirBtn, &QPushButton::clicked, this, &ClaudeProjectsDialog::onAddProjectDir);
    dirRow->addWidget(addDirBtn);

    auto *removeDirBtn = new QPushButton("-", this);
    removeDirBtn->setFixedWidth(30);
    removeDirBtn->setToolTip("Remove selected project directory");
    connect(removeDirBtn, &QPushButton::clicked, this, &ClaudeProjectsDialog::onRemoveProjectDir);
    dirRow->addWidget(removeDirBtn);

    auto *newProjBtn = new QPushButton("New Project...", this);
    newProjBtn->setToolTip("Create a new project folder and start Claude Code in it");
    connect(newProjBtn, &QPushButton::clicked, this, &ClaudeProjectsDialog::onNewProjectInDir);
    dirRow->addWidget(newProjBtn);

    mainLayout->addLayout(dirRow);

    // Top: horizontal splitter with project list and session list
    auto *topSplitter = new QSplitter(Qt::Horizontal, this);

    // -- Left: Project tree --
    auto *leftWidget = new QWidget(this);
    auto *leftLayout = new QVBoxLayout(leftWidget);
    leftLayout->setContentsMargins(0, 0, 0, 0);

    auto *projLabel = new QLabel("<b>Projects</b> (from Claude Code history)", this);
    projLabel->setStyleSheet("color: #7F849C; font-size: 11px; padding: 2px 0;");
    leftLayout->addWidget(projLabel);

    m_projectTree = new QTreeWidget(this);
    m_projectTree->setHeaderLabels({"Project", "Sessions", "Last Active"});
    m_projectTree->setRootIsDecorated(false);
    m_projectTree->setAlternatingRowColors(true);
    m_projectTree->header()->setStretchLastSection(false);
    m_projectTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_projectTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_projectTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_projectTree->setSelectionMode(QAbstractItemView::SingleSelection);
    connect(m_projectTree, &QTreeWidget::itemSelectionChanged,
            this, &ClaudeProjectsDialog::onProjectSelected);
    leftLayout->addWidget(m_projectTree);

    topSplitter->addWidget(leftWidget);

    // -- Right: Session tree --
    auto *rightWidget = new QWidget(this);
    auto *rightLayout = new QVBoxLayout(rightWidget);
    rightLayout->setContentsMargins(0, 0, 0, 0);

    auto *sessLabel = new QLabel("<b>Sessions</b>", this);
    sessLabel->setStyleSheet("color: #7F849C; font-size: 11px; padding: 2px 0;");
    rightLayout->addWidget(sessLabel);

    m_sessionTree = new QTreeWidget(this);
    m_sessionTree->setHeaderLabels({"Summary", "Name", "Date", "Size", "Status"});
    m_sessionTree->setRootIsDecorated(false);
    m_sessionTree->setAlternatingRowColors(true);
    m_sessionTree->header()->setStretchLastSection(false);
    m_sessionTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_sessionTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_sessionTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_sessionTree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_sessionTree->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_sessionTree->setSelectionMode(QAbstractItemView::SingleSelection);
    connect(m_sessionTree, &QTreeWidget::itemSelectionChanged,
            this, &ClaudeProjectsDialog::onSessionSelected);
    connect(m_sessionTree, &QTreeWidget::itemDoubleClicked,
            this, &ClaudeProjectsDialog::onSessionDoubleClicked);
    rightLayout->addWidget(m_sessionTree);

    topSplitter->addWidget(rightWidget);
    topSplitter->setSizes({350, 700});

    // Main vertical splitter: top (lists) + bottom (preview)
    auto *vertSplitter = new QSplitter(Qt::Vertical, this);
    vertSplitter->addWidget(topSplitter);

    // -- Bottom: Preview pane + memory --
    auto *bottomWidget = new QWidget(this);
    auto *bottomLayout = new QVBoxLayout(bottomWidget);
    bottomLayout->setContentsMargins(0, 0, 0, 0);

    m_previewPane = new QTextEdit(this);
    m_previewPane->setReadOnly(true);
    m_previewPane->setMaximumHeight(180);
    m_previewPane->setPlaceholderText("Select a session to preview...");
    bottomLayout->addWidget(m_previewPane);

    m_memoryLabel = new QLabel(this);
    m_memoryLabel->setWordWrap(true);
    m_memoryLabel->setStyleSheet("color: #7F849C; font-size: 11px; padding: 4px;");
    m_memoryLabel->hide();
    bottomLayout->addWidget(m_memoryLabel);

    vertSplitter->addWidget(bottomWidget);
    vertSplitter->setSizes({450, 200});

    mainLayout->addWidget(vertSplitter, 1);

    // -- Button row --
    auto *btnLayout = new QHBoxLayout();

    auto *refreshBtn = new QPushButton("Refresh", this);
    connect(refreshBtn, &QPushButton::clicked, this, &ClaudeProjectsDialog::refresh);
    btnLayout->addWidget(refreshBtn);

    btnLayout->addStretch();

    m_newBtn = new QPushButton("New Session", this);
    m_newBtn->setToolTip("Start a new Claude Code session in this project (cd <dir> && claude)");
    m_newBtn->setEnabled(false);
    connect(m_newBtn, &QPushButton::clicked, this, &ClaudeProjectsDialog::onNewSession);
    btnLayout->addWidget(m_newBtn);

    m_continueBtn = new QPushButton("Continue Latest", this);
    m_continueBtn->setToolTip("Continue the most recent session (claude --continue)");
    m_continueBtn->setEnabled(false);
    connect(m_continueBtn, &QPushButton::clicked, this, &ClaudeProjectsDialog::onContinue);
    btnLayout->addWidget(m_continueBtn);

    m_forkBtn = new QPushButton("Fork && Resume", this);
    m_forkBtn->setToolTip("Resume with a new session ID (claude --resume --fork-session)");
    m_forkBtn->setEnabled(false);
    connect(m_forkBtn, &QPushButton::clicked, this, &ClaudeProjectsDialog::onForkResume);
    btnLayout->addWidget(m_forkBtn);

    m_resumeBtn = new QPushButton("Resume Session", this);
    m_resumeBtn->setToolTip("Resume the selected session (claude --resume <id>)");
    m_resumeBtn->setEnabled(false);
    connect(m_resumeBtn, &QPushButton::clicked, this, &ClaudeProjectsDialog::onResume);
    btnLayout->addWidget(m_resumeBtn);

    mainLayout->addLayout(btnLayout);

    refreshProjectDirCombo();
    refresh();
}

void ClaudeProjectsDialog::refresh() {
    m_projects = m_integration->discoverProjects();
    populateProjects();
    m_sessionTree->clear();
    m_previewPane->clear();
    m_memoryLabel->hide();
    m_resumeBtn->setEnabled(false);
    m_continueBtn->setEnabled(false);
    m_forkBtn->setEnabled(false);
    m_newBtn->setEnabled(false);
}

void ClaudeProjectsDialog::refreshProjectDirCombo() {
    m_projectDirCombo->clear();
    QStringList dirs = m_config->claudeProjectDirs();
    for (const QString &dir : dirs)
        m_projectDirCombo->addItem(dir);
}

void ClaudeProjectsDialog::populateProjects() {
    m_projectTree->clear();
    for (int i = 0; i < m_projects.size(); ++i) {
        const auto &proj = m_projects[i];
        auto *item = new QTreeWidgetItem(m_projectTree);

        // Show the last path component as the display name, full path as tooltip
        QString displayName = proj.path.section('/', -1);
        if (displayName.isEmpty()) displayName = proj.path;
        item->setText(0, displayName);
        item->setToolTip(0, proj.path);

        item->setText(1, QString::number(proj.sessions.size()));
        item->setText(2, formatTimeAgo(proj.lastActivity));
        item->setData(0, Qt::UserRole, i); // project index

        // Bold the project name if it has active sessions
        bool hasActive = false;
        for (const auto &s : proj.sessions) {
            if (s.isActive) { hasActive = true; break; }
        }
        if (hasActive) {
            QFont f = item->font(0);
            f.setBold(true);
            item->setFont(0, f);
            item->setForeground(0, QColor(0xA6, 0xE3, 0xA1)); // green
        }
    }
}

void ClaudeProjectsDialog::populateSessions(int projectIndex) {
    m_sessionTree->clear();
    if (projectIndex < 0 || projectIndex >= m_projects.size()) return;

    const auto &proj = m_projects[projectIndex];
    for (int i = 0; i < proj.sessions.size(); ++i) {
        const auto &session = proj.sessions[i];
        auto *item = new QTreeWidgetItem(m_sessionTree);

        // Summary (first user message)
        QString summary = session.firstMessage;
        if (summary.isEmpty()) {
            // Lazy-load if not yet fetched
            summary = m_integration->sessionSummary(session.transcriptPath);
        }
        if (summary.isEmpty()) summary = "(empty session)";
        if (summary.length() > 80)
            summary = summary.left(80) + "...";
        item->setText(0, summary);
        item->setToolTip(0, session.firstMessage.isEmpty()
            ? session.sessionId : session.firstMessage);

        item->setText(1, session.name.isEmpty() ? "-" : session.name);
        item->setText(2, session.lastModified.toString("yyyy-MM-dd hh:mm"));
        item->setText(3, formatSize(session.sizeBytes));

        if (session.isActive) {
            item->setText(4, "ACTIVE");
            item->setForeground(4, QColor(0xA6, 0xE3, 0xA1));
            QFont f = item->font(4);
            f.setBold(true);
            item->setFont(4, f);
        }

        item->setData(0, Qt::UserRole, i);
    }
}

void ClaudeProjectsDialog::onProjectSelected() {
    auto items = m_projectTree->selectedItems();
    if (items.isEmpty()) return;

    int projIdx = items.first()->data(0, Qt::UserRole).toInt();
    populateSessions(projIdx);

    const auto &proj = m_projects[projIdx];
    if (!proj.memorySnippet.isEmpty()) {
        m_memoryLabel->setText("Memory: " + proj.memorySnippet.left(300));
        m_memoryLabel->show();
    } else {
        m_memoryLabel->hide();
    }

    m_continueBtn->setEnabled(!proj.sessions.isEmpty());
    m_newBtn->setEnabled(true);
    m_previewPane->clear();
}

void ClaudeProjectsDialog::onSessionSelected() {
    auto items = m_sessionTree->selectedItems();
    bool hasSel = !items.isEmpty();
    m_resumeBtn->setEnabled(hasSel);
    m_forkBtn->setEnabled(hasSel);

    if (!hasSel) return;

    auto projItems = m_projectTree->selectedItems();
    if (projItems.isEmpty()) return;
    int projIdx = projItems.first()->data(0, Qt::UserRole).toInt();
    int sessIdx = items.first()->data(0, Qt::UserRole).toInt();

    if (projIdx < m_projects.size() && sessIdx < m_projects[projIdx].sessions.size())
        showSessionPreview(m_projects[projIdx].sessions[sessIdx]);
}

void ClaudeProjectsDialog::onSessionDoubleClicked(QTreeWidgetItem *, int) {
    onResume();
}

void ClaudeProjectsDialog::showSessionPreview(const ClaudeSession &session) {
    QString html;

    html += QString("<p><b style='color:#89B4FA;'>Session:</b> %1</p>")
            .arg(session.sessionId.toHtmlEscaped());

    if (!session.name.isEmpty())
        html += QString("<p><b style='color:#F9E2AF;'>Name:</b> %1</p>")
                .arg(session.name.toHtmlEscaped());

    html += QString("<p><b style='color:#CDD6F4;'>Project:</b> %1</p>")
            .arg(session.projectPath.toHtmlEscaped());

    html += QString("<p><b style='color:#CDD6F4;'>Last modified:</b> %1</p>")
            .arg(session.lastModified.toString("yyyy-MM-dd hh:mm:ss"));

    html += QString("<p><b style='color:#CDD6F4;'>Size:</b> %1</p>")
            .arg(formatSize(session.sizeBytes));

    if (session.isActive)
        html += "<p><b style='color:#A6E3A1;'>Status: ACTIVE (running)</b></p>";

    if (!session.firstMessage.isEmpty())
        html += QString("<p><b style='color:#CDD6F4;'>First message:</b><br>%1</p>")
                .arg(session.firstMessage.toHtmlEscaped().replace("\n", "<br>"));

    m_previewPane->setHtml(html);
}

void ClaudeProjectsDialog::onResume() {
    auto projItems = m_projectTree->selectedItems();
    auto sessItems = m_sessionTree->selectedItems();
    if (projItems.isEmpty() || sessItems.isEmpty()) return;

    int projIdx = projItems.first()->data(0, Qt::UserRole).toInt();
    int sessIdx = sessItems.first()->data(0, Qt::UserRole).toInt();
    if (projIdx >= m_projects.size()) return;
    const auto &proj = m_projects[projIdx];
    if (sessIdx >= proj.sessions.size()) return;

    emit resumeSession(proj.path, proj.sessions[sessIdx].sessionId, false);
    accept();
}

void ClaudeProjectsDialog::onForkResume() {
    auto projItems = m_projectTree->selectedItems();
    auto sessItems = m_sessionTree->selectedItems();
    if (projItems.isEmpty() || sessItems.isEmpty()) return;

    int projIdx = projItems.first()->data(0, Qt::UserRole).toInt();
    int sessIdx = sessItems.first()->data(0, Qt::UserRole).toInt();
    if (projIdx >= m_projects.size()) return;
    const auto &proj = m_projects[projIdx];
    if (sessIdx >= proj.sessions.size()) return;

    emit resumeSession(proj.path, proj.sessions[sessIdx].sessionId, true);
    accept();
}

void ClaudeProjectsDialog::onContinue() {
    auto projItems = m_projectTree->selectedItems();
    if (projItems.isEmpty()) return;

    int projIdx = projItems.first()->data(0, Qt::UserRole).toInt();
    if (projIdx >= m_projects.size()) return;

    emit continueProject(m_projects[projIdx].path);
    accept();
}

void ClaudeProjectsDialog::onNewSession() {
    auto projItems = m_projectTree->selectedItems();
    if (projItems.isEmpty()) return;

    int projIdx = projItems.first()->data(0, Qt::UserRole).toInt();
    if (projIdx >= m_projects.size()) return;

    emit newSession(m_projects[projIdx].path);
    accept();
}

void ClaudeProjectsDialog::onNewProjectInDir() {
    // Get the selected project directory (or pick one)
    QString baseDir;
    if (m_projectDirCombo->count() > 0) {
        baseDir = m_projectDirCombo->currentText();
    }

    if (baseDir.isEmpty()) {
        baseDir = QFileDialog::getExistingDirectory(this,
            "Select parent directory for the new project", QDir::homePath());
        if (baseDir.isEmpty()) return;
    }

    // Ask for new project name
    bool ok;
    QString projectName = QInputDialog::getText(this, "New Project",
        QString("Project name (will be created in %1):").arg(baseDir),
        QLineEdit::Normal, "", &ok);
    if (!ok || projectName.trimmed().isEmpty()) return;

    QString projectPath = baseDir + "/" + projectName.trimmed();

    // Create the directory
    if (!QDir().mkpath(projectPath)) {
        QMessageBox::warning(this, "Error",
            "Failed to create directory: " + projectPath);
        return;
    }

    emit newSession(projectPath);
    accept();
}

void ClaudeProjectsDialog::onAddProjectDir() {
    QString dir = QFileDialog::getExistingDirectory(this,
        "Select a project directory", QDir::homePath());
    if (dir.isEmpty()) return;

    QStringList dirs = m_config->claudeProjectDirs();
    if (!dirs.contains(dir)) {
        dirs.append(dir);
        m_config->setClaudeProjectDirs(dirs);
    }
    refreshProjectDirCombo();
}

void ClaudeProjectsDialog::onRemoveProjectDir() {
    int idx = m_projectDirCombo->currentIndex();
    if (idx < 0) return;

    QStringList dirs = m_config->claudeProjectDirs();
    if (idx < dirs.size()) {
        dirs.removeAt(idx);
        m_config->setClaudeProjectDirs(dirs);
    }
    refreshProjectDirCombo();
}

QString ClaudeProjectsDialog::formatSize(qint64 bytes) const {
    if (bytes < 1024) return QString("%1 B").arg(bytes);
    if (bytes < 1024 * 1024) return QString("%1 KB").arg(bytes / 1024);
    if (bytes < 1024 * 1024 * 1024)
        return QString("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
    return QString("%1 GB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
}

QString ClaudeProjectsDialog::formatTimeAgo(const QDateTime &dt) const {
    if (!dt.isValid()) return "-";
    qint64 secs = dt.secsTo(QDateTime::currentDateTime());
    if (secs < 60) return "just now";
    if (secs < 3600) return QString("%1m ago").arg(secs / 60);
    if (secs < 86400) return QString("%1h ago").arg(secs / 3600);
    if (secs < 86400 * 30) return QString("%1d ago").arg(secs / 86400);
    return dt.toString("yyyy-MM-dd");
}
