// proc_stat_fields. Contract: spec.md beside this file.

#include <gtest/gtest.h>

#include "procstat.h"

#include <QFile>

#include <unistd.h>

#include "../../_support/srcgrep.h"

TEST(ProcStatFields, TpgidIsTheSixthFieldAfterComm) {
    // pid (comm) state ppid pgrp session tty_nr tpgid flags
    EXPECT_EQ(procstat::tpgid(QStringLiteral("100 (bash) S 1 100 100 34816 4242 4194560")), 4242);
    EXPECT_EQ(procstat::tpgid(QStringLiteral("100 (a) b) S 1 100 100 34816 4242 0")), 4242);
    EXPECT_EQ(procstat::tpgid(QStringLiteral("100 (bash) S 1 100 100 34816")), -1);
    EXPECT_EQ(procstat::tpgid(QStringLiteral("no paren here")), -1);
}

TEST(ProcStatFields, FieldOrderMatchesTheKernel) {
    QFile f(QStringLiteral("/proc/self/stat"));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QStringList fields = procstat::fieldsAfterComm(QString::fromUtf8(f.readAll()));
    ASSERT_GT(fields.size(), 5);
    EXPECT_EQ(fields[2].toInt(), static_cast<int>(getpgrp()));
    EXPECT_EQ(fields[3].toInt(), static_cast<int>(getsid(0)));
}

TEST(ProcStatFields, PpidAndStarttimeReadTheirFields) {
    // pid (comm) state ppid ... 19 more fields ... starttime
    const QString line = QStringLiteral(
        "100 (a) b) S 77 100 100 34816 4242 0 1 2 3 4 5 6 7 8 20 0 1 0 987654 9");
    EXPECT_EQ(procstat::ppid(line), 77);
    EXPECT_EQ(procstat::starttime(line), std::optional<qulonglong>(987654));
    EXPECT_EQ(procstat::ppid(QStringLiteral("no paren here")), 0);
    EXPECT_FALSE(procstat::starttime(QStringLiteral("100 (bash) S 1 100")).has_value());
    EXPECT_FALSE(procstat::starttime(QString(line).replace(QStringLiteral("987654"),
                                                          QStringLiteral("x"))).has_value());

    QFile f(QStringLiteral("/proc/self/stat"));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QString self = QString::fromUtf8(f.readAll());
    EXPECT_EQ(procstat::ppid(self), static_cast<qint64>(getppid()));
    EXPECT_TRUE(procstat::starttime(self).has_value());
}

TEST(ProcStatFields, ForegroundProcessUsesTheHelper) {
    const QString src = QString::fromStdString(
        ants_test::slurpFile(SRC_TERMINALWIDGET_PATH));
    const qsizetype at = src.indexOf(QStringLiteral("QString TerminalWidget::foregroundProcess() const {"));
    ASSERT_GE(at, 0) << "foregroundProcess not found";
    const QString body = src.mid(at, src.indexOf(QStringLiteral("\n}\n"), at) - at);
    EXPECT_TRUE(body.contains(QStringLiteral("procstat::tpgid("))) << body.toStdString();
    EXPECT_FALSE(body.contains(QStringLiteral("fields["))) << body.toStdString();
}
