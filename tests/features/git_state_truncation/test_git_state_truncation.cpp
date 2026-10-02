// Feature-conformance test for the ANTS-5098 git_state truncation finding.
// Contract: tests/features/git_state_truncation/spec.md

#include <gtest/gtest.h>

#include "gitwrap.h"
#include "remotecontrol.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace {

bool git(const QString &cwd, const QStringList &args) {
    QProcess p;
    p.setWorkingDirectory(cwd);
    p.start(QStandardPaths::findExecutable(QStringLiteral("git")), args);
    return p.waitForFinished(120000) && p.exitCode() == 0;
}

// Enough long-named files that one line each passes GitWrap's stdout cap.
QSet<QString> seed(const QString &root) {
    const QString stem(100, QLatin1Char('x'));
    QSet<QString> names;
    int bytes = 0;
    for (int i = 0; bytes < GitWrap::kStdoutCapBytes * 3 / 2; ++i) {
        const QString name = QStringLiteral("%1-%2.txt").arg(stem).arg(i, 6, 10, QLatin1Char('0'));
        QFile f(QDir(root).filePath(name));
        if (!f.open(QIODevice::WriteOnly)) { ADD_FAILURE() << "cannot write " << name.toStdString(); break; }
        f.write("x\n");
        names.insert(name);
        bytes += int(name.size()) + 8;
    }
    return names;
}

QJsonObject gitState(const QString &root, const QJsonObject &args) {
    RemoteControl rc(nullptr);
    QJsonObject req = args;
    req[QStringLiteral("caller_cwd")] = root;
    return rc.cmdGitState(req).object();
}

void expectWholePaths(const QJsonObject &env, const QSet<QString> &names) {
    const QJsonArray files = env.value(QStringLiteral("files")).toArray();
    ASSERT_FALSE(files.isEmpty());
    EXPECT_LT(files.size(), names.size()) << "the read was meant to pass the cap";
    for (const auto &v : files) {
        const QString p = v.toObject().value(QStringLiteral("path")).toString();
        EXPECT_TRUE(names.contains(p)) << "a cut path was emitted: " << p.toStdString();
    }
}

}  // namespace

TEST(GitStateTruncation, StatusAndNumstatSayTheyWereCut) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = tmp.path();
    ASSERT_TRUE(git(root, {QStringLiteral("init"), QStringLiteral("-q")}));
    const QSet<QString> names = seed(root);

    // INV-1 — status, every file untracked.
    const QJsonObject st = gitState(root, {{QStringLiteral("op"), QStringLiteral("status")}});
    ASSERT_TRUE(st.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(st).toJson().toStdString();
    EXPECT_TRUE(st.value(QStringLiteral("truncated")).toBool());
    expectWholePaths(st, names);

    // INV-2 — numstat over the staged files.
    ASSERT_TRUE(git(root, {QStringLiteral("add"), QStringLiteral("-A")}));
    const QJsonObject df = gitState(root, {{QStringLiteral("op"), QStringLiteral("diff")},
                                           {QStringLiteral("staged"), true}});
    ASSERT_TRUE(df.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(df).toJson().left(2000).toStdString();
    EXPECT_TRUE(df.value(QStringLiteral("truncated")).toBool());
    expectWholePaths(df, names);
    EXPECT_EQ(df.value(QStringLiteral("totals")).toObject().value(QStringLiteral("files")).toInt(),
              df.value(QStringLiteral("files")).toArray().size());
}
