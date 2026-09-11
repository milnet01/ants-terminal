// ANTS-5031 — feature-conformance test; see spec.md. Behavioural against
// SessionManager and TerminalGrid, headless.

#include "sessionmanager.h"
#include "terminalgrid.h"
#include "vtparser.h"

#include "../../_support/xdg_guard.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

namespace {

void feed(TerminalGrid &grid, const QByteArray &bytes) {
    VtParser parser([&grid](const VtAction &act) {
        grid.processAction(act);
    });
    parser.feed(bytes.constData(), bytes.size());
}

QByteArray numberedLines(int n) {
    QByteArray out;
    for (int i = 0; i < n; ++i)
        out += "line-" + QByteArray::number(i) + "\r\n";
    return out;
}

QString rowText(const std::vector<Cell> &cells) {
    QString out;
    for (const Cell &c : cells)
        out += QChar(static_cast<char16_t>(c.codepoint ? c.codepoint : ' '));
    return out.trimmed();
}

// Removes every file in the sessions directory that starts with `stem`.
void removeMatching(QDir dir, const QString &stem) {
    for (const QString &f : dir.entryList({stem + QStringLiteral("*")}, QDir::Files))
        dir.remove(f);
}

}  // namespace

// INV-A — a blob that fails partway through leaves the grid untouched.
TEST(SessionRestoreRefusal, InvARefusedRestoreLeavesGridUntouched) {
    TerminalGrid source(10, 40);
    feed(source, numberedLines(200));
    const QByteArray envelope = SessionManager::serialize(&source);

    // Strip the envelope (magic, version, SHA-256, length) and cut the inner
    // stream in half. A legacy blob carries no checksum, so the cut is found
    // only while parsing, after the header and dimensions have been read.
    constexpr int kEnvelopeHeader = 4 + 4 + 32 + 4;
    QByteArray raw = qUncompress(envelope.mid(kEnvelopeHeader));
    ASSERT_FALSE(raw.isEmpty());
    raw.truncate(raw.size() / 2);
    const QByteArray legacyCut = qCompress(raw, 6);

    TerminalGrid target(24, 80);
    ASSERT_FALSE(SessionManager::restore(&target, legacyCut));
    EXPECT_EQ(target.rows(), 24) << "a refused restore resized the grid";
    EXPECT_EQ(target.cols(), 80) << "a refused restore resized the grid";
    EXPECT_EQ(target.scrollbackSize(), 0)
        << "a refused restore pushed scrollback lines";
}

// INV-B — loadSession keeps a file that restore refuses.
TEST(SessionRestoreRefusal, InvBRefusedFileIsKeptAside) {
    ants_test::XdgGuard xdg;
    xdg.setTestMode(true);  // restored on scope exit
    const QString id = QStringLiteral("ants-5031-refused");
    const QString path = SessionManager::sessionPath(id);
    ASSERT_FALSE(path.isEmpty());
    const QDir dir = QFileInfo(path).absoluteDir();
    const QString stem = QFileInfo(path).fileName();
    removeMatching(dir, stem);  // a test-mode directory outlives the run

    const QByteArray junk("not a session file");
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(junk);
    }

    TerminalGrid grid(24, 80);
    EXPECT_FALSE(SessionManager::loadSession(id, &grid));

    const QStringList aside =
        dir.entryList({stem + QStringLiteral(".corrupt-*")}, QDir::Files);
    ASSERT_EQ(aside.size(), 1) << "the refused file was not kept";
    QFile kept(dir.filePath(aside.first()));
    ASSERT_TRUE(kept.open(QIODevice::ReadOnly));
    EXPECT_EQ(kept.readAll(), junk);
    removeMatching(dir, stem);
}

// INV-C — serialize writes only what restore accepts, keeping the newest
// scrollback lines.
TEST(SessionRestoreRefusal, InvCSaveKeepsNewestLinesWithinCaps) {
    TerminalGrid source(5, 80);
    source.setMaxScrollback(5000);
    feed(source, numberedLines(3000));
    ASSERT_GT(source.scrollbackSize(), 2900);

    // 100 KiB of stream holds roughly ninety 80-cell lines.
    constexpr qint64 kCap = 100LL * 1024;
    const QByteArray blob = SessionManager::serialize(&source, {}, {}, kCap, kCap);
    EXPECT_LE(blob.size(), kCap);

    TerminalGrid target(5, 80);
    target.setMaxScrollback(5000);
    ASSERT_TRUE(SessionManager::restore(&target, blob));
    EXPECT_GT(target.scrollbackSize(), 0);
    EXPECT_LT(target.scrollbackSize(), source.scrollbackSize())
        << "the stream cap was ignored";
    EXPECT_EQ(rowText(target.scrollbackLine(target.scrollbackSize() - 1)),
              rowText(source.scrollbackLine(source.scrollbackSize() - 1)))
        << "the kept lines must be the newest ones";
}
