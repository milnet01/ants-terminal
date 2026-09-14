// ANTS-1677 INV-5 — the two readers of the audit rule ids see one set.
//
// tests/audit_self_test.sh and the product's audit_fixture_coverage check each
// extract every addGrepCheck("<id>" from the AuditDialog source, and both stay
// silent when they extract nothing. ANTS-1044 moves those calls into another
// file, so a reader still pointed at the old file would pass while checking
// nothing. This case runs both readers over one copy of the tree and requires
// the same non-empty id set. Contract: spec.md beside this file.

#include "auditdialog.h"
#include "config.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

namespace {

// The AuditDialog class's files, by the glob the shell readers use:
// src/auditdialog.cpp, then src/auditdialog_*.cpp.
QStringList classSources() {
    const QDir src(QStringLiteral(SRC_DIR));
    QStringList out;
    if (src.exists(QStringLiteral("auditdialog.cpp")))
        out << src.filePath(QStringLiteral("auditdialog.cpp"));
    for (const QString &name : src.entryList({QStringLiteral("auditdialog_*.cpp")},
                                             QDir::Files, QDir::Name))
        out << src.filePath(name);
    return out;
}

bool copyInto(const QString &from, const QString &to) {
    QDir().mkpath(QFileInfo(to).absolutePath());
    return QFile::copy(from, to);
}

struct BashRun {
    QByteArray out;
    QByteArray err;
};

BashRun runBash(const QStringList &args, const QString &cwd) {
    QProcess p;
    p.setWorkingDirectory(cwd);
    p.start(QStringLiteral("/bin/bash"), args);
    p.waitForFinished(30000);
    return {p.readAllStandardOutput(), p.readAllStandardError()};
}

QString sortedJoin(const QSet<QString> &ids) {
    QStringList list(ids.begin(), ids.end());
    list.sort();
    return list.join(QStringLiteral(", "));
}

}  // namespace

TEST(AuditFixtureReaders, SelfTestAndRuntimeCheckSeeTheSameIds) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();

    const QStringList sources = classSources();
    ASSERT_FALSE(sources.isEmpty()) << "no AuditDialog source under " << SRC_DIR;
    for (const QString &s : sources) {
        ASSERT_TRUE(copyInto(s, root + QStringLiteral("/src/") + QFileInfo(s).fileName()))
            << s.toStdString();
    }
    const QString script = root + QStringLiteral("/tests/audit_self_test.sh");
    ASSERT_TRUE(copyInto(QStringLiteral(SRC_DIR "/../tests/audit_self_test.sh"), script));
    // The copy has no tests/audit_fixtures/, so the runtime check reports
    // every id it extracts rather than only the ones missing a fixture.
    ASSERT_FALSE(QFileInfo::exists(root + QStringLiteral("/tests/audit_fixtures")));

    // Reader 1 — the script's fixture-coverage extraction.
    const BashRun listed = runBash({script, QStringLiteral("--list-rule-ids")}, root);
    QSet<QString> scriptIds;
    for (const QByteArray &line : listed.out.split('\n')) {
        const QString id = QString::fromUtf8(line).trimmed();
        if (!id.isEmpty()) scriptIds.insert(id);
    }

    // Reader 2 — the audit_fixture_coverage command, as the catalogue holds it.
    Config cfg;
    AuditDialog dlg(root, nullptr, &cfg);
    QString command;
    for (const AuditCheck &c : dlg.checksForTest()) {
        if (c.id == QLatin1String("audit_fixture_coverage")) command = c.command;
    }
    ASSERT_FALSE(command.isEmpty()) << "audit_fixture_coverage is not in the catalogue";

    const BashRun reported = runBash({QStringLiteral("-c"), command}, root);
    // Custom delimiter: the pattern contains `)"`, which closes a plain R"( … )".
    static const QRegularExpression reFinding(
        QStringLiteral(R"RX(rule "([^"]+)" has no fixture directory)RX"));
    QSet<QString> runtimeIds;
    auto it = reFinding.globalMatch(QString::fromUtf8(reported.out));
    while (it.hasNext()) runtimeIds.insert(it.next().captured(1));

    EXPECT_FALSE(scriptIds.isEmpty())
        << "audit_self_test.sh --list-rule-ids extracted no ids; stderr: "
        << listed.err.constData();
    EXPECT_FALSE(runtimeIds.isEmpty())
        << "the audit_fixture_coverage command extracted no ids; stderr: "
        << reported.err.constData();
    EXPECT_TRUE(scriptIds == runtimeIds)
        << "only audit_self_test.sh: " << sortedJoin(scriptIds - runtimeIds).toStdString()
        << "\nonly audit_fixture_coverage: "
        << sortedJoin(runtimeIds - scriptIds).toStdString();
}
