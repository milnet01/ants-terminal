#include "claudetasklistdialog.h"

#include "claudetasklist.h"
#include "themes.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

// Status icon glyphs map to JSON-canonical status names.
QString statusIcon(const QString &status) {
    if (status == QStringLiteral("completed"))    return QStringLiteral("✓");
    if (status == QStringLiteral("in_progress"))  return QStringLiteral("◐");
    return QStringLiteral("☐");  // pending or unknown
}

// One-line render for a row. Subject is bold. Description (or
// activeForm fallback) follows after an em-dash, truncated to keep
// the row legible.
QString rowText(const ClaudeTask &t) {
    constexpr int kMaxDesc = 200;
    QString detail = t.description.isEmpty() ? t.activeForm : t.description;
    if (detail.size() > kMaxDesc)
        detail = detail.left(kMaxDesc - 1) + QChar(0x2026);  // …
    if (detail.isEmpty())
        return QStringLiteral("%1  %2").arg(statusIcon(t.status), t.subject);
    return QStringLiteral("%1  %2 — %3")
        .arg(statusIcon(t.status), t.subject, detail);
}

} // namespace

ClaudeTaskListDialog::ClaudeTaskListDialog(ClaudeTaskListTracker *tracker,
                                           QString themeName,
                                           QWidget *parent)
    : QDialog(parent),
      m_tracker(tracker),
      m_themeName(std::move(themeName)) {
    setObjectName(QStringLiteral("claudeTaskListDialog"));
    setWindowTitle(tr("Task List"));
    resize(700, 480);
    setAttribute(Qt::WA_DeleteOnClose);

    auto *layout = new QVBoxLayout(this);

    auto *header = new QLabel(this);
    header->setObjectName(QStringLiteral("taskListHeader"));
    m_header = header;

    auto *list = new QListWidget(this);
    list->setObjectName(QStringLiteral("taskListItems"));
    list->setSelectionMode(QAbstractItemView::NoSelection);
    list->setFocusPolicy(Qt::NoFocus);
    m_list = list;

    auto *btnRow = new QHBoxLayout;
    auto *closeBtn = new QPushButton(tr("Close"), this);
    btnRow->addStretch();
    btnRow->addWidget(closeBtn);
    // Plain QPushButton + clicked → close, not QDialogButtonBox
    // (QTBUG-79126 mitigation per debug_wayland_modal_dialog).
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);

    layout->addWidget(header);
    layout->addWidget(list, 1);
    layout->addLayout(btnRow);

    if (m_tracker) {
        connect(m_tracker, &ClaudeTaskListTracker::tasksChanged,
                this, &ClaudeTaskListDialog::rebuild);
    }

    rebuild();
}

void ClaudeTaskListDialog::rebuild() {
    if (!m_list || !m_header || !m_tracker) return;

    const Theme &th = Themes::byName(m_themeName);
    const auto &tasks = m_tracker->tasks();
    const int total       = tasks.size();
    const int completed   = m_tracker->completedCount();
    const int inProgress  = m_tracker->inProgressCount();
    const int pending     = m_tracker->pendingCount();

    // Header uses the user's vocabulary (running / outstanding / done).
    if (total == 0) {
        m_header->setText(tr("No active task list."));
    } else {
        m_header->setText(
            tr("%1 task%2 — %3 done, %4 running, %5 outstanding")
                .arg(total)
                .arg(total == 1 ? QString() : QStringLiteral("s"))
                .arg(completed)
                .arg(inProgress)
                .arg(pending));
    }
    m_header->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; "
                                           "padding: 4px 0;")
                                .arg(th.textSecondary.name()));

    m_list->clear();
    for (const auto &t : tasks) {
        auto *item = new QListWidgetItem(rowText(t), m_list);
        QColor fg = th.textPrimary;
        if (t.status == QStringLiteral("completed"))
            fg = th.textSecondary;
        item->setForeground(fg);
    }
}
