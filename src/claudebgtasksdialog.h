#pragma once

#include <QDialog>
#include <QFileSystemWatcher>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <memory>

class QLabel;
class QTextEdit;
class QPushButton;
class ClaudeBgTaskTracker;

// Live-tail dialog for Claude Code background tasks. Mirrors the
// Review Changes dialog's update model:
//
//   • QFileSystemWatcher on each task's .output file plus the tracker's
//     transcript path (to pick up new starts / completions).
//   • Debounce watcher signals through a 200 ms QTimer so a noisy
//     stdout doesn't thrash the renderer.
//   • Skip-identical-HTML guard via a shared lastHtml cache.
//   • Capture vbar/hbar value before setHtml, restore after with a
//     std::min clamp — preserves scroll position across live refreshes.
class ClaudeBgTasksDialog : public QDialog {
    Q_OBJECT

public:
    explicit ClaudeBgTasksDialog(ClaudeBgTaskTracker *tracker,
                                 const QString &themeName,
                                 QWidget *parent = nullptr);

private slots:
    void scheduleRebuild();
    void rebuild();

private:
    void rewatch();

    ClaudeBgTaskTracker *m_tracker;
    QString m_themeName;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QPointer<QTextEdit> m_viewer;
    QPointer<QLabel> m_liveStatus;
    std::shared_ptr<QString> m_lastHtml;
};
