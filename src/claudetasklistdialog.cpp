#include "claudetasklistdialog.h"

#include "claudetasklist.h"
#include "dialogchrome.h"
#include "themes.h"

#include <QAction>
#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>

namespace {

// Status icon glyphs map to JSON-canonical status names.
QString statusIcon(const QString &status) {
    if (status == QStringLiteral("completed"))    return QStringLiteral("✓");
    if (status == QStringLiteral("in_progress"))  return QStringLiteral("◐");
    return QStringLiteral("☐");  // pending or unknown
}

// ANTS-1639 — detect raw tool-input XML-ish markup leaked from a
// malformed upstream TaskCreate call. Three observed shapes:
//   * Closing tags:  `</subject>`, `</description>`
//   * Opening tags with a `name="…"` attr: `<parameter name="…">`
//   * Bare opening tags: `<subject>`, `<description>`
// We MUST NOT match every `<…>` (legitimate task subjects can mention
// `<repo>` or `<branch>` in angle-bracket placeholders). The
// rule-of-thumb here is: only flag when the markup looks exactly
// like an unescaped fragment of the Claude Code tool-input wrapper.
bool looksMalformedMarkup(const QString &s) {
    static const QRegularExpression rx(QStringLiteral(
        "</(?:subject|description|parameter)>"
        "|<parameter\\s+name=\""
        "|<(?:subject|description)>"));
    return rx.match(s).hasMatch();
}

// One-line render for a row. Subject is bold. Description (or
// activeForm fallback) follows after an em-dash, truncated to keep
// the row legible. ANTS-1639 — prepend "[malformed] " when either
// the subject or description bears raw tool-input markup, so the
// user can spot the upstream encoding bug without stripping the
// evidence (per the entry's design preference (b) over (a)).
QString rowText(const ClaudeTask &t) {
    constexpr int kMaxDesc = 200;
    QString detail = t.description.isEmpty() ? t.activeForm : t.description;
    if (detail.size() > kMaxDesc)
        detail = detail.left(kMaxDesc - 1) + QChar(0x2026);  // …
    const bool malformed = looksMalformedMarkup(t.subject)
                         || looksMalformedMarkup(detail);
    const QString prefix = malformed ? QStringLiteral("[malformed] ")
                                     : QString();
    if (detail.isEmpty())
        return QStringLiteral("%1  %2%3")
            .arg(statusIcon(t.status), prefix, t.subject);
    return QStringLiteral("%1  %2%3 — %4")
        .arg(statusIcon(t.status), prefix, t.subject, detail);
}

// ANTS-1638 — UserRole keys for structured task data stored alongside
// the rendered display string. Lets the context-menu slot recover
// the task's id / status / raw fields without re-walking the
// tracker's task vector and matching by row text (which is lossy
// after the [malformed] prefix + truncation).
constexpr int kRoleTaskId          = Qt::UserRole + 1;
constexpr int kRoleStatus          = Qt::UserRole + 2;
constexpr int kRoleSubject         = Qt::UserRole + 3;
constexpr int kRoleDescriptionFull = Qt::UserRole + 4;

// ANTS-1638 — "Copy as markdown" payload: GFM-style task list bullet,
// status emoji rendered as `[ ]` / `[x]` checkbox. Keeps the long
// description intact (no 200-char truncation).
QString markdownForRow(const QString &status,
                       const QString &subject,
                       const QString &description) {
    const QString box = (status == QStringLiteral("completed"))
        ? QStringLiteral("[x]") : QStringLiteral("[ ]");
    if (description.isEmpty()) {
        return QStringLiteral("- %1 %2").arg(box, subject);
    }
    return QStringLiteral("- %1 %2 — %3").arg(box, subject, description);
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

    // ANTS-1242 — frameless + theme-aware TitleBar.
    auto chrome = DialogChrome::install(this, m_themeName);
    QWidget *content = chrome.contentArea;

    auto *layout = new QVBoxLayout(content);

    auto *header = new QLabel(this);
    header->setObjectName(QStringLiteral("taskListHeader"));
    m_header = header;

    auto *list = new QListWidget(this);
    list->setObjectName(QStringLiteral("taskListItems"));
    list->setSelectionMode(QAbstractItemView::NoSelection);
    list->setFocusPolicy(Qt::NoFocus);
    // ANTS-1638 — wire up a custom context menu on row right-click.
    // Slot itemAt()s the position and pops a QMenu with Copy
    // variants. NoSelection + NoFocus stay as-is — the menu reads
    // from UserRole-stashed data, not from a selected-row state.
    list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(list, &QWidget::customContextMenuRequested,
            this, &ClaudeTaskListDialog::showRowContextMenu);
    // ANTS-1328: wrap long task subjects to the dialog's width
    // instead of letting them overflow horizontally. `setWordWrap`
    // toggles per-item wrapping; `setTextElideMode(Qt::ElideNone)`
    // disables the default mid-text elision so the wrap can actually
    // claim a second line. `setHorizontalScrollBarPolicy` removes
    // the horizontal scrollbar entirely — vertical-only scrolling
    // matches Claude Code's own task-list rendering.
    list->setWordWrap(true);
    list->setTextElideMode(Qt::ElideNone);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setResizeMode(QListView::Adjust);
    // ANTS-1329: 3 px of vertical padding around each item for
    // breathing room — the wrapped multi-line subjects
    // (ANTS-1328) ran together visually without separation.
    // setSpacing applies uniformly above + below each item, so
    // the total gap between neighbours is 2 × spacing = 6 px
    // (user-tuned 2026-05-14: 6 → 4 → 3; first two read too
    // generous, settled at 3 for a tight-but-readable gap).
    list->setSpacing(3);
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
        // ANTS-1638 — stash the structured fields so the context
        // menu can read raw values without re-walking the tracker.
        // Use the long description (not the 200-char truncated one
        // that rowText() built) so Copy-as-markdown is lossless.
        item->setData(kRoleTaskId,  t.id);
        item->setData(kRoleStatus,  t.status);
        item->setData(kRoleSubject, t.subject);
        item->setData(kRoleDescriptionFull,
                      t.description.isEmpty() ? t.activeForm : t.description);
    }
}

// ANTS-1638 — right-click context menu handler. itemAt() against the
// list's viewport-local pos (the position emitted by
// customContextMenuRequested is in viewport coordinates already).
// Returns early when the cursor isn't over a row, so the menu only
// pops on a real target. Actions copy the variant to the clipboard
// via QGuiApplication::clipboard().
void ClaudeTaskListDialog::showRowContextMenu(const QPoint &pos) {
    if (!m_list) return;
    QListWidgetItem *item = m_list->itemAt(pos);
    if (!item) return;

    const QString rendered    = item->text();
    const QString taskId      = item->data(kRoleTaskId).toString();
    const QString status      = item->data(kRoleStatus).toString();
    const QString subject     = item->data(kRoleSubject).toString();
    const QString description = item->data(kRoleDescriptionFull).toString();

    QMenu menu(this);
    auto *copyText = menu.addAction(tr("Copy task text"));
    auto *copyMarkdown = menu.addAction(tr("Copy as markdown"));
    QAction *copyId = nullptr;
    if (!taskId.isEmpty()) {
        copyId = menu.addAction(tr("Copy task ID"));
    }

    QAction *picked = menu.exec(m_list->viewport()->mapToGlobal(pos));
    if (!picked) return;
    QClipboard *cb = QGuiApplication::clipboard();
    if (!cb) return;
    if (picked == copyText) {
        cb->setText(rendered);
    } else if (picked == copyMarkdown) {
        cb->setText(markdownForRow(status, subject, description));
    } else if (copyId && picked == copyId) {
        cb->setText(taskId);
    }
}
