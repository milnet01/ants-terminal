#pragma once

// ANTS-1236 — Roadmap keyboard-shortcut cheatsheet overlay.
//
// A small sub-dialog parented to RoadmapDialog, opened by pressing `?`
// on the parent. Renders the file-scope `kRoadmapShortcuts[]` table in
// `roadmapdialog.cpp` as a two-column QTableWidget (Shortcut / Action).
//
// Native widgets only — no HTML / QTextBrowser — so QAccessibleTableInterface
// reaches screen readers without the metadata fragility the ANTS-1235
// status-emoji label work surfaced.
//
// Spec: docs/specs/ANTS-1236.md.

#include <QDialog>
#include <QString>

class QKeyEvent;

class RoadmapShortcutsDialog : public QDialog {
    Q_OBJECT

public:
    RoadmapShortcutsDialog(const QString &themeName,
                           QWidget *parent = nullptr);

protected:
    void keyPressEvent(QKeyEvent *event) override;
};
