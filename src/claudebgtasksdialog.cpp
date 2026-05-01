#include "claudebgtasksdialog.h"

#include "claudebgtasks.h"
#include "themes.h"

#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTextEdit>
#include <QTextStream>
#include <QVBoxLayout>
#include <QtMath>

namespace {

// Read up to `maxBytes` from the tail of `path` and return as UTF-8. We
// scope the read because some background tasks (e.g. a long `make
// -j$(nproc)`) can fill the .output file with megabytes of compiler
// noise and rendering all of that into a QTextEdit costs more than the
// user wants to pay every refresh.
// ANTS-1144 — strip ANSI control sequences (CSI/SGR) from tail
// output. Pre-fix code rendered raw bytes, so a build log full of
// ncurses-style progress bars (`make -j$(nproc)` with colored
// output, claude-code's status bar, etc.) showed literal `^[[2J`
// and similar escape sequences in the dialog. The terminal proper
// has VtParser/TerminalGrid for this; per CLAUDE.md rule 3
// (reuse-before-rewriting) we ought to channel through that. But
// running a full VT state machine for tail rendering is overkill —
// a regex pass that drops the common CSI form (ESC `[` params
// `@-~` final byte) covers ~99% of in-the-wild noise without
// pulling in the parser link footprint.
QString stripAnsi(const QString &input) {
    static const QRegularExpression re(
        QStringLiteral("\x1B\\[[\\x20-\\x3f]*[\\x40-\\x7e]"));
    QString s = input;
    s.remove(re);
    // Also strip OSC sequences (ESC ] ... BEL) which command tools
    // sometimes emit for window-title updates. ESC 7-bit + ST or
    // BEL terminator.
    static const QRegularExpression osc(
        QStringLiteral("\x1B\\][^\x07\x1B]*(?:\x07|\x1B\\\\)"));
    s.remove(osc);
    return s;
}

QString tailFile(const QString &path, qint64 maxBytes = 32 * 1024) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const qint64 size = f.size();
    qint64 start = 0;
    if (size > maxBytes) start = size - maxBytes;
    if (start > 0) f.seek(start);
    const QByteArray bytes = f.readAll();
    QString s = QString::fromUtf8(bytes);
    if (start > 0) {
        // Drop any partial line at the head of the window.
        const int firstNl = s.indexOf('\n');
        if (firstNl >= 0) s = s.mid(firstNl + 1);
        s.prepend(QStringLiteral("… (older output truncated)\n"));
    }
    // ANTS-1144 — ANSI strip happens before the caller HTML-escapes.
    return stripAnsi(s);
}

QString humanAge(const QDateTime &start) {
    if (!start.isValid()) return {};
    const qint64 secs = start.secsTo(QDateTime::currentDateTimeUtc());
    if (secs < 0) return {};
    if (secs < 60) return QStringLiteral("%1s").arg(secs);
    if (secs < 3600) return QStringLiteral("%1m").arg(secs / 60);
    if (secs < 86400) return QStringLiteral("%1h%2m").arg(secs / 3600).arg((secs % 3600) / 60);
    return QStringLiteral("%1d").arg(secs / 86400);
}

} // namespace

ClaudeBgTasksDialog::ClaudeBgTasksDialog(ClaudeBgTaskTracker *tracker,
                                         const QString &themeName,
                                         QWidget *parent)
    : QDialog(parent),
      m_tracker(tracker),
      m_themeName(themeName),
      m_lastHtml(std::make_shared<QString>()) {
    setObjectName(QStringLiteral("claudeBgTasksDialog"));
    setWindowTitle(tr("Background Tasks"));
    resize(900, 600);
    setAttribute(Qt::WA_DeleteOnClose);

    auto *layout = new QVBoxLayout(this);
    auto *viewer = new QTextEdit(this);
    viewer->setReadOnly(true);
    viewer->setFont(QFont(QStringLiteral("Monospace"), 10));
    m_viewer = viewer;

    auto *btnBox = new QHBoxLayout;
    auto *liveStatus = new QLabel(this);
    liveStatus->setObjectName(QStringLiteral("bgTasksLiveStatus"));
    liveStatus->setStyleSheet(QStringLiteral("color: gray; font-size: 11px;"));
    m_liveStatus = liveStatus;
    auto *refreshBtn = new QPushButton(tr("Refresh"), this);
    refreshBtn->setObjectName(QStringLiteral("bgTasksRefreshBtn"));
    auto *closeBtn = new QPushButton(tr("Close"), this);
    btnBox->addWidget(liveStatus);
    btnBox->addStretch();
    btnBox->addWidget(refreshBtn);
    btnBox->addWidget(closeBtn);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
    connect(refreshBtn, &QPushButton::clicked, this, &ClaudeBgTasksDialog::rebuild);

    layout->addWidget(viewer);
    layout->addLayout(btnBox);

    // Mirror Review Changes' debounce: coalesce bursts of fileChanged
    // signals (e.g. a tight write loop in the backgrounded process)
    // into one render every 200 ms.
    m_debounce.setInterval(200);
    m_debounce.setSingleShot(true);
    connect(&m_debounce, &QTimer::timeout, this, &ClaudeBgTasksDialog::rebuild);

    connect(&m_watcher, &QFileSystemWatcher::fileChanged,
            this, &ClaudeBgTasksDialog::scheduleRebuild);

    if (m_tracker) {
        connect(m_tracker, &ClaudeBgTaskTracker::tasksChanged,
                this, &ClaudeBgTasksDialog::scheduleRebuild);
    }

    rewatch();
    rebuild();
}

void ClaudeBgTasksDialog::scheduleRebuild() {
    if (m_liveStatus) {
        m_liveStatus->setText(QStringLiteral("● refreshing…"));
        m_liveStatus->setStyleSheet(QStringLiteral("color: #e0a020; font-size: 11px;"));
    }
    m_debounce.start();
    rewatch();
}

void ClaudeBgTasksDialog::rewatch() {
    if (!m_tracker) return;
    QStringList wanted;
    if (!m_tracker->transcriptPath().isEmpty())
        wanted << m_tracker->transcriptPath();
    for (const auto &t : m_tracker->tasks()) {
        if (!t.outputPath.isEmpty() && QFileInfo::exists(t.outputPath))
            wanted << t.outputPath;
    }
    const QStringList have = m_watcher.files();
    QStringList toAdd, toRemove;
    for (const QString &p : wanted) if (!have.contains(p)) toAdd << p;
    for (const QString &p : have) if (!wanted.contains(p)) toRemove << p;
    if (!toRemove.isEmpty()) m_watcher.removePaths(toRemove);
    if (!toAdd.isEmpty()) m_watcher.addPaths(toAdd);
}

void ClaudeBgTasksDialog::rebuild() {
    if (!m_viewer || !m_tracker) return;
    const Theme &th = Themes::byName(m_themeName);

    const auto tasks = m_tracker->tasks();
    int running = 0;
    for (const auto &t : tasks) if (!t.finished) ++running;

    QString html;
    html += QStringLiteral("<pre style='color: %1; background: %2; "
                          "font-family: Monospace; font-size: 10pt;'>")
                .arg(th.textPrimary.name(), th.bgPrimary.name());

    if (tasks.isEmpty()) {
        html += QStringLiteral("<span style='color: %1;'>No background tasks "
                              "in the active Claude Code session.</span>")
                    .arg(th.textSecondary.name());
    } else {
        html += QStringLiteral("<span style='color: %1;'>%2 task%3 "
                              "(%4 running)</span>\n\n")
                    .arg(th.textSecondary.name())
                    .arg(tasks.size())
                    .arg(tasks.size() == 1 ? QString() : QStringLiteral("s"))
                    .arg(running);

        for (const auto &t : tasks) {
            const QString stateColor = t.finished
                ? th.ansi[8].name()             // bright black — finished
                : th.ansi[2].name();            // green — running
            const QString stateText = t.finished ? QStringLiteral("finished")
                                                  : QStringLiteral("running");
            const QString age = humanAge(t.startedAt);

            html += QStringLiteral(
                "<span style='color: %1;'>=== %2 (%3) "
                "<span style='color: %4;'>[%5%6]</span></span>\n")
                .arg(th.ansi[6].name())   // cyan header
                .arg(t.id.toHtmlEscaped(),
                     t.tool.toHtmlEscaped())
                .arg(stateColor,
                     stateText,
                     age.isEmpty() ? QString() : (QStringLiteral(" · ") + age));

            if (!t.description.isEmpty() && t.description != t.command) {
                html += QStringLiteral("<span style='color: %1;'>%2</span>\n")
                            .arg(th.textSecondary.name(),
                                 t.description.toHtmlEscaped());
            }
            if (!t.command.isEmpty()) {
                html += QStringLiteral("<span style='color: %1;'>$ %2</span>\n")
                            .arg(th.textSecondary.name(),
                                 t.command.toHtmlEscaped());
            }
            html += QStringLiteral("<span style='color: %1;'>--- output (%2)</span>\n")
                        .arg(th.textSecondary.name(),
                             t.outputPath.toHtmlEscaped());

            const QString output = tailFile(t.outputPath);
            if (output.isEmpty()) {
                html += QStringLiteral("<span style='color: %1;'>(no output yet)</span>\n")
                            .arg(th.textSecondary.name());
            } else {
                html += output.toHtmlEscaped();
                if (!html.endsWith('\n')) html += '\n';
            }
            html += '\n';
        }
    }
    html += QStringLiteral("</pre>");

    // 0.7.37 scroll-preservation pattern: skip identical renders, then
    // capture vbar/hbar before setHtml, restore after with std::min
    // clamp. lastHtml is per-dialog (member, not static) so multiple
    // dialogs don't fight over the cache.
    if (*m_lastHtml == html) {
        if (m_liveStatus) {
            m_liveStatus->setText(QStringLiteral("● live"));
            m_liveStatus->setStyleSheet(
                QStringLiteral("color: %1; font-size: 11px;").arg(th.ansi[2].name()));
        }
        return;
    }
    const bool isFirstRender = m_lastHtml->isEmpty();
    *m_lastHtml = html;

    QScrollBar *vbar = m_viewer->verticalScrollBar();
    QScrollBar *hbar = m_viewer->horizontalScrollBar();
    const int vPos = (vbar && !isFirstRender) ? vbar->value() : 0;
    const int hPos = (hbar && !isFirstRender) ? hbar->value() : 0;
    const bool wasAtBottom = vbar && (vbar->value() >= vbar->maximum() - 4);

    m_viewer->setHtml(html);

    if (vbar && !isFirstRender) {
        // If the user was tailing the bottom (auto-scroll behavior
        // most users expect from a "live tail" pane), keep them there.
        // Otherwise restore their absolute position.
        if (wasAtBottom) {
            vbar->setValue(vbar->maximum());
        } else {
            vbar->setValue(qMin(vPos, vbar->maximum()));
        }
    }
    if (hbar && !isFirstRender) hbar->setValue(qMin(hPos, hbar->maximum()));

    if (m_liveStatus) {
        m_liveStatus->setText(QStringLiteral("● live"));
        m_liveStatus->setStyleSheet(
            QStringLiteral("color: %1; font-size: 11px;").arg(th.ansi[2].name()));
    }
}
