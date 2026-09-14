// ANTS-5151 — every owner-only permission result is used.
//
// Scrapes src/ for a bare `setOwnerOnlyPerms(...);` statement. The compiler
// warns on a discarded [[nodiscard]] result, but a warning does not stop a
// build, so this is what turns a new discarded call into a failure.
// See tests/features/owner_only_result_used/spec.md.

#include <gtest/gtest.h>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#ifndef ANTS_SOURCE_DIR
#  error "ANTS_SOURCE_DIR compile definition required"
#endif

namespace {

QString readText(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

}  // namespace

// INV-1 — no call discards the result.
TEST(OwnerOnlyResultUsed, Inv1NoBareCall) {
    const QString srcDir = QString::fromUtf8(ANTS_SOURCE_DIR "/src");
    static const QRegularExpression bare(
        QStringLiteral(R"(^\s*setOwnerOnlyPerms\s*\([^;]*\)\s*;)"));
    QStringList hits;
    int scanned = 0;
    QDirIterator it(srcDir, {QStringLiteral("*.cpp"), QStringLiteral("*.h")},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        ++scanned;
        const QStringList lines = readText(path).split(QLatin1Char('\n'));
        for (int i = 0; i < lines.size(); ++i) {
            if (bare.match(lines.at(i)).hasMatch())
                hits << QStringLiteral("%1:%2")
                            .arg(path.mid(srcDir.size() + 1)).arg(i + 1);
        }
    }
    ASSERT_GT(scanned, 50) << "setup: src/ was not found or is nearly empty";
    EXPECT_TRUE(hits.isEmpty())
        << "INV-1: setOwnerOnlyPerms result discarded at: "
        << hits.join(QStringLiteral(", ")).toStdString()
        << " — take one half of the policy in src/secureio.h";
}

// INV-2 — the policy lives in secureio.h.
TEST(OwnerOnlyResultUsed, Inv2PolicyInSecureio) {
    const QString h =
        readText(QString::fromUtf8(ANTS_SOURCE_DIR "/src/secureio.h"));
    ASSERT_FALSE(h.isEmpty());
    EXPECT_TRUE(h.contains(QStringLiteral(
        "[[nodiscard]] inline bool setOwnerOnlyPerms(QFileDevice &f)")));
    EXPECT_TRUE(h.contains(QStringLiteral(
        "[[nodiscard]] inline bool setOwnerOnlyPerms(const QString &path)")));
    EXPECT_TRUE(h.contains(QStringLiteral("inline void warnNotOwnerOnly(")));
}
