#pragma once

#include <QDialog>
#include <QPointer>
#include <QString>

class QLabel;
class QListWidget;
class ClaudeTaskListTracker;

// Read-only view of the focused tab's Claude Code task list.
//
// Mirrors ClaudeBgTasksDialog's lifetime model:
//   * Stores theme name + raw tracker pointer (lifetime guaranteed
//     by ClaudeStatusBarController which outlives MainWindow).
//   * Connects tracker->tasksChanged → rebuild() for live updates.
//   * Non-modal, no QDialogButtonBox (Wayland QTBUG-79126
//     mitigation; see debug_wayland_modal_dialog memory).
class ClaudeTaskListDialog : public QDialog {
    Q_OBJECT

public:
    explicit ClaudeTaskListDialog(ClaudeTaskListTracker *tracker,
                                  QString themeName,
                                  QWidget *parent = nullptr);

private slots:
    void rebuild();

private:
    ClaudeTaskListTracker *m_tracker;
    QString m_themeName;
    QPointer<QLabel> m_header;
    QPointer<QListWidget> m_list;
};
