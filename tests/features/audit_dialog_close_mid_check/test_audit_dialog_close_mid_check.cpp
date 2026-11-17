// ANTS-6539 — closing the audit dialog mid-check. Contract: spec.md beside
// this file.

#include "auditdialog.h"
#include "config.h"

#include <gtest/gtest.h>
#include "../../_support/xdg_guard.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QPointer>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>

// INV-1 — no process signal fires while the dialog is being destroyed.
TEST(AuditDialogCloseMidCheck, NoProcessSignalDuringDestruction) {
    QTemporaryDir project;
    ASSERT_TRUE(project.isValid());
    ants_test::XdgGuard env;
    Config cfg;
    auto *dlg = new AuditDialog(project.path(), nullptr, &cfg);

    const auto procs = dlg->findChildren<QProcess *>(QString(), Qt::FindDirectChildrenOnly);
    ASSERT_FALSE(procs.isEmpty()) << "the dialog owns no QProcess";
    QPointer<QProcess> proc = procs.first();
    proc->start(QStringLiteral("sleep"), {QStringLiteral("30")});
    ASSERT_TRUE(proc->waitForStarted(5000));
    QSignalSpy finished(proc.data(), &QProcess::finished);

    delete dlg;
    EXPECT_EQ(finished.count(), 0)
        << "the process emitted finished while the dialog was being destroyed";

    // The released process is deleted once its kill is reaped.
    const QDeadlineTimer deadline(5000);
    while (proc && !deadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    EXPECT_TRUE(proc.isNull()) << "the released process was never deleted";
}
