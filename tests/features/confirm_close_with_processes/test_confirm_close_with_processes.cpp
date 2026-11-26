// Feature-conformance test for spec.md — ANTS-1102 confirm-on-close.
//
// Source-grep against config.{h,cpp}, mainwindow.{h,cpp},
// settingsdialog.{h,cpp}. The dialog itself uses real /proc reads
// which can't be exercised without spawning a real PTY tree, so we
// validate the wire-up shape via grep — the same pattern the rest
// of the project uses for MainWindow-touching invariants.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"

#include <QFile>
#include <QString>

#include <gtest/gtest.h>

ANTS_TEST_SCOPE();

namespace {

QString readFileOrFail(const char *macroPath) {
    QFile f(QString::fromUtf8(macroPath));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        expect(false, "source-open",
               QStringLiteral("cannot read %1").arg(macroPath));
        return {};
    }
    QString s = QString::fromUtf8(f.readAll());
    f.close();
    return s;
}

// ANTS-1476 — delegate to the shared brace-counting, string/comment-aware
// extractor. The old local version found the body end via indexOf("\n}"),
// which truncates at the first column-0 `}` inside the function (a nested
// block, a lambda, a raw-string), so a body containing one would silently
// be cut short and the INV `.contains()` checks could miss real tokens.
// slurpFunctionBody returns the matched `{...}` body (signature excluded);
// every call site below searches body content, never the signature.
QString extractFunctionBody(const QString &src, const QString &signature) {
    return QString::fromStdString(ants_test::slurpFunctionBody(
        src.toStdString(), signature.toStdString()));
}

}  // namespace

TEST(ConfirmCloseWithProcesses, Main) {
    expect_reset();
    const QString configH = readFileOrFail(SRC_CONFIG_H_PATH);
    const QString configCpp = readFileOrFail(SRC_CONFIG_CPP_PATH);
    const QString mwCpp = QString::fromStdString(ants_test::slurpMainWindow());
    const QString sdH = readFileOrFail(SRC_SETTINGSDIALOG_H_PATH);
    const QString sdCpp = readFileOrFail(SRC_SETTINGSDIALOG_CPP_PATH);

    if (configH.isEmpty() || configCpp.isEmpty() || mwCpp.isEmpty()
            || sdH.isEmpty() || sdCpp.isEmpty()) {
        FAIL();
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

    // INV-9: the dialog uses the Wayland-correct non-modal pattern. Its
    // shape lives in buildCloseConfirmDialog, which the window-close dialog
    // shares (ANTS-6600); the tab wrapper must build through it.
    const QString dlgBody = extractFunctionBody(mwCpp,
        QStringLiteral(
            "void MainWindow::showCloseTabConfirmDialog"));
    expect(!dlgBody.isEmpty(),
           "INV-9-precondition/showCloseTabConfirmDialog-located");
    const QString builderBody = extractFunctionBody(mwCpp,
        QStringLiteral("CloseConfirmDialog buildCloseConfirmDialog("));
    expect(!builderBody.isEmpty(),
           "INV-9-precondition/buildCloseConfirmDialog-located");
    expect(dlgBody.contains(QStringLiteral("buildCloseConfirmDialog(")),
           "INV-9/tab-dialog-built-by-the-shared-builder");
    expect(builderBody.contains(QStringLiteral("new QDialog(")) &&
           builderBody.contains(QStringLiteral("WA_DeleteOnClose")) &&
           !builderBody.contains(QStringLiteral("setModal(")) &&
           !builderBody.contains(QStringLiteral("QDialogButtonBox")) &&
           builderBody.contains(QStringLiteral("new QPushButton(")),
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

    // ANTS-5123: the close dialogs conform to dialogs.md D1-D4 via
    // DialogChrome::install, in the builder both share (ANTS-6600).
    // Comment-stripped so a doc comment describing the fix can't satisfy
    // this the wrong way (window_close_confirm's INV-1 takes the same
    // precaution).
    const std::string mwStripped =
        ants_test::stripComments(mwCpp.toStdString());
    const std::string tabDlgBody = ants_test::slurpFunctionBody(
        mwStripped, "void MainWindow::showCloseTabConfirmDialog(");
    const std::string winDlgBody = ants_test::slurpFunctionBody(
        mwStripped, "void MainWindow::showCloseWindowConfirmDialog(");
    const std::string builder = ants_test::slurpFunctionBody(
        mwStripped, "CloseConfirmDialog buildCloseConfirmDialog(");
    expect(!tabDlgBody.empty() && !winDlgBody.empty() && !builder.empty(),
           "INV-12-precondition/dialog-bodies-located-stripped");

    // INV-12: DialogChrome::install(...) with resizable=true, and a size
    // key per dialog: the key is built from `what`, which the tab and
    // window wrappers pass differently.
    const auto installPos = builder.find("DialogChrome::install(");
    const auto keyPos = builder.find(
        "QStringLiteral(\"Close%1ConfirmDialog\").arg(what)");
    expect(installPos != std::string::npos,
           "INV-12/calls-DialogChrome-install");
    expect(keyPos != std::string::npos && installPos < keyPos,
           "INV-12/size-key-built-from-what");
    expect(installPos < keyPos && keyPos != std::string::npos &&
               builder.substr(installPos, keyPos - installPos).find("true")
                   != std::string::npos,
           "INV-12/resizable-true");
    expect(tabDlgBody.find("QStringLiteral(\"Tab\")") != std::string::npos &&
           winDlgBody.find("QStringLiteral(\"Window\")") != std::string::npos,
           "INV-12/size-key-distinct-from-window-dialog");

    // INV-13: layout built on the chrome's content area, not directly
    // on the QDialog.
    expect(builder.find("QVBoxLayout(chrome.contentArea)")
               != std::string::npos,
           "INV-13/layout-on-chrome-contentArea");
    expect(builder.find("QVBoxLayout(dlg)") == std::string::npos,
           "INV-13/layout-not-directly-on-dlg");

    // INV-14 (guard): stays non-modal after adopting DialogChrome, so
    // installing the chrome cannot silently reintroduce a modal dialog.
    for (const std::string *body : {&builder, &tabDlgBody}) {
        expect(body->find("exec(") == std::string::npos, "INV-14/no-exec");
        expect(body->find("setModal(true)") == std::string::npos,
               "INV-14/no-setModal-true");
    }
    expect(builder.find("dlg->show()") != std::string::npos,
           "INV-14/calls-show");

    ASSERT_EQ(0, expect_finish());
}

