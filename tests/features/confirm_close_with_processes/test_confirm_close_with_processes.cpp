// Feature-conformance test for spec.md — ANTS-1102 confirm-on-close.
//
// Source-grep against config.{h,cpp}, mainwindow.{h,cpp},
// settingsdialog.{h,cpp}. The dialog itself uses real /proc reads
// which can't be exercised without spawning a real PTY tree, so we
// validate the wire-up shape via grep — the same pattern the rest
// of the project uses for MainWindow-touching invariants.

#include <QCoreApplication>
#include <QFile>
#include <QString>

#include <cstdio>

namespace {

int g_failures = 0;

void expect(bool cond, const char *label, const QString &detail = {}) {
    if (cond) {
        std::fprintf(stderr, "[PASS] %s\n", label);
    } else {
        std::fprintf(stderr, "[FAIL] %s  %s\n", label,
                     qUtf8Printable(detail));
        ++g_failures;
    }
}

QString readFileOrFail(const char *macroPath) {
    QFile f(QString::fromUtf8(macroPath));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::fprintf(stderr,
                     "[FAIL] source-open: cannot read %s\n", macroPath);
        return {};
    }
    QString s = QString::fromUtf8(f.readAll());
    f.close();
    return s;
}

QString extractFunctionBody(const QString &src, const QString &signature) {
    const int start = src.indexOf(signature);
    if (start < 0) return {};
    const int pos = src.indexOf(QStringLiteral("\n}"), start);
    if (pos < 0) return {};
    return src.mid(start, pos - start + 2);
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    const QString configH = readFileOrFail(SRC_CONFIG_H_PATH);
    const QString configCpp = readFileOrFail(SRC_CONFIG_CPP_PATH);
    const QString mwCpp = readFileOrFail(SRC_MAINWINDOW_PATH);
    const QString sdH = readFileOrFail(SRC_SETTINGSDIALOG_H_PATH);
    const QString sdCpp = readFileOrFail(SRC_SETTINGSDIALOG_CPP_PATH);

    if (configH.isEmpty() || configCpp.isEmpty() || mwCpp.isEmpty()
            || sdH.isEmpty() || sdCpp.isEmpty()) {
        return 1;
    }

    // INV-1: declarations in config.h
    expect(configH.contains(
               QStringLiteral("bool confirmCloseWithProcesses() const;")),
           "INV-1/config.h-getter");
    expect(configH.contains(
               QStringLiteral(
                   "void setConfirmCloseWithProcesses(bool enabled);")),
           "INV-1/config.h-setter");

    // INV-2: defaults to true in config.cpp impl.
    expect(configCpp.contains(
               QStringLiteral(
                   "m_data.value(\"confirm_close_with_processes\")"
                   ".toBool(true)")),
           "INV-2/default-true");

    // INV-3: setter uses storeIfChanged idempotency.
    const QString setterBody = extractFunctionBody(configCpp,
        QStringLiteral(
            "void Config::setConfirmCloseWithProcesses(bool enabled)"));
    expect(!setterBody.isEmpty(),
           "INV-3-precondition/setter-located");
    expect(setterBody.contains(
               QStringLiteral(
                   "storeIfChanged(\"confirm_close_with_processes\"")),
           "INV-3/setter-storeIfChanged");

    // INV-4 + INV-5: helpers exist in mainwindow.cpp's anonymous ns.
    expect(mwCpp.contains(
               QStringLiteral(
                   "QString firstNonShellDescendant(pid_t shellPid)")),
           "INV-4/firstNonShellDescendant-defined");
    expect(mwCpp.contains(
               QStringLiteral(
                   "/proc/%1/task/%1/children")) &&
           mwCpp.contains(
               QStringLiteral("/proc/%1/comm")),
           "INV-4/walks-proc-children-and-comm");
    expect(mwCpp.contains(
               QStringLiteral("safeShellNames()")) &&
           mwCpp.contains(QStringLiteral("\"bash\"")) &&
           mwCpp.contains(QStringLiteral("\"zsh\"")) &&
           mwCpp.contains(QStringLiteral("\"fish\"")) &&
           mwCpp.contains(QStringLiteral("\"sh\"")),
           "INV-5/safeShellNames-includes-baseline");

    // INV-6 + INV-7: closeTab calls firstNonShellDescendant when
    // confirmCloseWithProcesses() is on, and routes to the dialog
    // helper without falling through to the teardown.
    const QString closeTabBody = extractFunctionBody(mwCpp,
        QStringLiteral("void MainWindow::closeTab(int index)"));
    expect(!closeTabBody.isEmpty(),
           "INV-6-precondition/closeTab-located");
    expect(closeTabBody.contains(
               QStringLiteral("m_config.confirmCloseWithProcesses()")) &&
           closeTabBody.contains(
               QStringLiteral("firstNonShellDescendant(")) &&
           closeTabBody.contains(
               QStringLiteral("term->shellPid() > 0")),
           "INV-6/closeTab-probes-when-config-on");
    expect(closeTabBody.contains(
               QStringLiteral("showCloseTabConfirmDialog(")) &&
           closeTabBody.contains(QStringLiteral("return;")),
           "INV-7/closeTab-routes-to-dialog-and-returns");
    // INV-7 stronger form: closeTab must not push to m_closedTabs
    // (that's performTabClose's job).
    expect(!closeTabBody.contains(QStringLiteral("m_closedTabs.prepend(")),
           "INV-7/closeTab-no-direct-push-to-undo-stack");

    // INV-8: performTabClose is the teardown helper, and pushes the
    // undo-close info.
    const QString perfBody = extractFunctionBody(mwCpp,
        QStringLiteral("void MainWindow::performTabClose(int index)"));
    expect(!perfBody.isEmpty(),
           "INV-8-precondition/performTabClose-located");
    expect(perfBody.contains(QStringLiteral("m_closedTabs.prepend(")) &&
           perfBody.contains(QStringLiteral("removeTab(")) &&
           perfBody.contains(QStringLiteral("deleteLater()")),
           "INV-8/performTabClose-is-the-teardown");

    // INV-9: dialog uses the Wayland-correct non-modal pattern.
    const QString dlgBody = extractFunctionBody(mwCpp,
        QStringLiteral(
            "void MainWindow::showCloseTabConfirmDialog"));
    expect(!dlgBody.isEmpty(),
           "INV-9-precondition/showCloseTabConfirmDialog-located");
    expect(dlgBody.contains(QStringLiteral("new QDialog(this)")) &&
           dlgBody.contains(QStringLiteral("WA_DeleteOnClose")) &&
           !dlgBody.contains(QStringLiteral("setModal(")) &&
           !dlgBody.contains(QStringLiteral("QDialogButtonBox")) &&
           dlgBody.contains(QStringLiteral("new QPushButton(")),
           "INV-9/wayland-correct-pattern");

    // INV-10: Don't-ask-again checkbox flips the config.
    expect(dlgBody.contains(
               QStringLiteral(
                   "m_config.setConfirmCloseWithProcesses(false)")),
           "INV-10/dont-ask-again-flips-config");

    // INV-11: SettingsDialog wire-up.
    expect(sdH.contains(
               QStringLiteral("QCheckBox *m_confirmCloseWithProcesses")),
           "INV-11/settingsdialog.h-member");
    expect(sdCpp.contains(
               QStringLiteral(
                   "m_confirmCloseWithProcesses = new QCheckBox(")),
           "INV-11/settingsdialog-constructs-checkbox");
    expect(sdCpp.contains(
               QStringLiteral(
                   "m_confirmCloseWithProcesses->setChecked"
                   "(m_config->confirmCloseWithProcesses())")),
           "INV-11/loadFromConfig-reads");
    expect(sdCpp.contains(
               QStringLiteral(
                   "m_config->setConfirmCloseWithProcesses("
                   "m_confirmCloseWithProcesses->isChecked())")),
           "INV-11/applySettings-writes");
    expect(sdCpp.contains(
               QStringLiteral(
                   "m_confirmCloseWithProcesses->setChecked(true)")),
           "INV-11/restore-defaults-true");

    if (g_failures) {
        std::fprintf(stderr, "\n%d invariant(s) failed\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nall invariants hold\n");
    return 0;
}
