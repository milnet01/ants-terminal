#pragma once

#include <QObject>

class QEvent;

// 0.7.58 (2026-04-30 ANTS-1054 follow-up) — runtime-toggleable dialog
// spawn tracer. When active, an instance is installed as an event
// filter on the QApplication; every top-level QWidget / QDialog
// `Show` event is logged to stderr (and to the `events` debug-log
// category when enabled) with className, objectName, windowTitle,
// parent class+objectName, and geometry.
//
// Two activation paths share one implementation:
//
//   1. Env var at startup: `ANTS_TRACE_DIALOGS=1 ants-terminal …`
//      (main.cpp calls setActive(true) before the GUI is built).
//
//   2. Runtime toggle from Tools → Debug Mode → "Trace dialog show
//      events" — checkable menu action wired to setActive().
//
// The state is process-global so a single tracer instance covers all
// windows and tabs. setActive() is idempotent — repeated true/false
// calls collapse to one install / uninstall.
class DialogShowTracer : public QObject {
public:
    explicit DialogShowTracer(QObject *parent = nullptr);

    bool eventFilter(QObject *obj, QEvent *ev) override;

    // Install (on=true) or uninstall (on=false) the tracer on the
    // current QApplication. No-op if the state already matches.
    // Safe to call before MainWindow exists (used by main.cpp on env-
    // var path) and from menu actions (used by mainwindow.cpp).
    static void setActive(bool on);

    // Whether the tracer is currently installed.
    static bool active();
};
