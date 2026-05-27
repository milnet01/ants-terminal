// ANTS-1894 INV-7 — defaultLedgerPath under XDG_CACHE_HOME, mode 0600,
// one \n-terminated JSON per line.
#include <gtest/gtest.h>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTemporaryDir>
#include "modelnearmissledger.h"

namespace NM = ModelNearMissLedger;

namespace {

NM::Record makeRecord(const QString &proj = QStringLiteral("/p")) {
    NM::Record r;
    r.ts              = QStringLiteral("2026-05-27T09:30:00Z");
    r.sessionId       = QStringLiteral("s1");
    r.project         = proj;
    r.currentTier     = QStringLiteral("opus");
    r.recommendedTier = QStringLiteral("haiku");
    r.blockedBy << QStringLiteral("composer_not_empty");
    r.composerEmpty   = false;
    r.focusedState    = QStringLiteral("thinking");
    r.ticksTargetStable = 1;
    r.dwellMs           = 45000;
    return r;
}

}  // namespace

TEST(ModelNearMissLedger, Inv7DefaultPathUnderXdgCache) {
    QTemporaryDir cacheDir;
    ASSERT_TRUE(cacheDir.isValid());
    qputenv("XDG_CACHE_HOME", cacheDir.path().toUtf8());
    // QStandardPaths caches; reset so the env var takes effect.
    QStandardPaths::setTestModeEnabled(false);

    const QString path = NM::defaultLedgerPath();
    EXPECT_TRUE(path.endsWith(QStringLiteral("/ants-terminal/model-switch-nearmiss.jsonl")));
}

TEST(ModelNearMissLedger, Inv7AppendCreatesOwnerOnlyFile) {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("nearmiss.jsonl"));
    ASSERT_TRUE(NM::appendRecord(path, makeRecord()));

    const QFileDevice::Permissions p = QFile(path).permissions();
    EXPECT_TRUE(p.testFlag(QFileDevice::ReadOwner));
    EXPECT_TRUE(p.testFlag(QFileDevice::WriteOwner));
    EXPECT_FALSE(p.testFlag(QFileDevice::ReadGroup));
    EXPECT_FALSE(p.testFlag(QFileDevice::ReadOther));
}

TEST(ModelNearMissLedger, Inv7OneJsonPerLine) {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("nearmiss.jsonl"));
    ASSERT_TRUE(NM::appendRecord(path, makeRecord(QStringLiteral("/p1"))));
    ASSERT_TRUE(NM::appendRecord(path, makeRecord(QStringLiteral("/p2"))));

    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray content = f.readAll();
    const QList<QByteArray> lines = content.split('\n');
    // Trailing newline → split yields a final empty element. Filter.
    int nonEmpty = 0;
    for (const QByteArray &ln : lines) {
        if (ln.trimmed().isEmpty()) continue;
        ++nonEmpty;
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(ln, &err);
        EXPECT_EQ(err.error, QJsonParseError::NoError);
        EXPECT_TRUE(doc.isObject());
        // No leading whitespace
        EXPECT_FALSE(ln.startsWith(' '));
    }
    EXPECT_EQ(nonEmpty, 2);
}
