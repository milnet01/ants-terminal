// ANTS-5225 — feature-conformance test; see spec.md. INV-1, INV-2 and INV-6
// are behavioural on SessionManager; INV-3, INV-4, INV-5 and INV-7 scrape the
// MainWindow and RemoteControl sources.

#include "sessionmanager.h"
#include "terminalgrid.h"
#include "vtparser.h"

#include "../../_support/srcgrep.h"
#include "../../_support/xdg_guard.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDataStream>
#include <QDateTime>
#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include <string>
#include <vector>

namespace {

void feed(TerminalGrid &grid, const QByteArray &bytes) {
    VtParser parser([&grid](const VtAction &act) { grid.processAction(act); });
    parser.feed(bytes.constData(), bytes.size());
}

// A grid with scrollback, combining marks and an OSC 0 title.
QByteArray savedBlob(const QString &cwd, const QString &pin) {
    TerminalGrid grid(6, 20);
    feed(grid, "\x1b]0;saved-title\x07");
    for (int i = 0; i < 40; ++i)
        feed(grid, "line-" + QByteArray::number(i) + " e\xcc\x81\r\n");
    return SessionManager::serialize(&grid, cwd, pin);
}

// A legacy (unenveloped) blob built by hand in serializeStream()'s layout,
// so each field can be pushed past the cap restore() applies to it.
struct Stream {
    uint32_t magic = 0x414E5453;
    uint32_t version = 3;
    int32_t rows = 2, cols = 3;
    int32_t sbLines = 1;       // scrollback line count written
    int32_t sbCells = 3;       // cells per scrollback line
    int32_t combEntries = 1;   // combining entries per line
    int32_t marks = 1;         // marks per entry
    int32_t screenRows = -1;   // -1: rows
    bool writeLines = true;    // false: counts only, for an over-cap count
    QString title = QStringLiteral("hand-title");
    QString cwd = QStringLiteral("/hand/cwd");
    QString pin = QStringLiteral("hand-pin");
    bool withCwd = true, withPin = true;
    int truncateTo = -1;       // cut the stream to this many bytes

    QByteArray build() const {
        QByteArray raw;
        QDataStream out(&raw, QIODevice::WriteOnly);
        out.setVersion(QDataStream::Qt_6_0);
        out << magic << version << rows << cols << int32_t(0) << int32_t(0);
        auto line = [&](int32_t cells, bool wrapped) {
            out << cells;
            if (wrapped) out << false;
            for (int i = 0; i < cells; ++i)
                out << uint32_t('a') << QRgb(0) << QRgb(0) << uint8_t(0);
            out << combEntries;
            for (int j = 0; j < combEntries; ++j) {
                out << int32_t(j) << marks;
                for (int k = 0; k < marks; ++k) out << uint32_t(0x301);
            }
        };
        out << sbLines;
        if (writeLines)
            for (int i = 0; i < sbLines; ++i) line(sbCells, true);
        const int32_t screen = screenRows < 0 ? rows : screenRows;
        out << screen;
        if (writeLines)
            for (int r = 0; r < screen; ++r) line(cols, false);
        out << title;
        if (withCwd) out << cwd;
        if (withPin) out << pin;
        if (truncateTo >= 0) raw.truncate(truncateTo);
        return qCompress(raw, 6);
    }
};

struct Read { bool ok; QString title, cwd, pin; };

Read viaRestore(const QByteArray &data) {
    TerminalGrid grid(24, 80);
    Read r;
    r.ok = SessionManager::restore(&grid, data, &r.cwd, &r.pin);
    r.title = grid.windowTitle();
    return r;
}

Read viaPeek(const QByteArray &data) {
    SessionPeek p;
    const bool ok = SessionManager::peek(data, &p);
    return {ok, p.title, p.cwd, p.pinnedTitle};
}

// Envelope offsets: magic, version, SHA-256, payload length, payload.
constexpr int kEnvVersionAt = 4;
constexpr int kEnvLengthAt = 4 + 4 + 32;

QByteArray withByte(QByteArray b, int at, char v) { b[at] = v; return b; }

std::string mainWindow() { return ants_test::stripComments(ants_test::slurpMainWindow()); }

std::string body(const std::string &src, const char *sig) {
    return ants_test::slurpFunctionBody(src, sig);
}

}  // namespace

// INV-1 and INV-2 — peek accepts and refuses exactly what restore does, and
// on acceptance returns the same title, cwd and pin.
TEST(LazyTabRestore, Inv1Inv2PeekMatchesRestore) {
    const QByteArray real = savedBlob(QStringLiteral("/saved/cwd"),
                                      QStringLiteral("saved-pin"));
    Stream v2; v2.version = 2;                         // carries a trailing pin
    Stream v1; v1.version = 1; v1.withCwd = false; v1.withPin = false;

    struct Case { const char *name; QByteArray data; bool accepted; };
    std::vector<Case> cases = {
        {"real V3 blob", real, true},
        {"real blob, no cwd or pin", savedBlob({}, {}), true},
        {"hand V3", Stream().build(), true},
        {"hand V2 with trailing pin bytes", v2.build(), true},
        {"hand V1", v1.build(), true},
        {"envelope version", withByte(real, kEnvVersionAt + 3, 2), false},
        {"SHA mismatch", withByte(real, real.size() - 1, char(real.back() ^ 1)), false},
        {"envelope length", withByte(real, kEnvLengthAt + 3, char(real[kEnvLengthAt + 3] + 1)), false},
        {"envelope length past the file cap", withByte(real, kEnvLengthAt, char(0x7f)), false},
        {"payload under 4 bytes", QByteArray("ab"), false},
        {"claimed length past MAX_UNCOMPRESSED",
         QByteArray("\x7f\xff\xff\xff", 4) + QByteArray(64, 'x'), false},
        {"claimed length past the ratio bound",
         QByteArray("\x01\x00\x00\x00", 4) + QByteArray(64, 'x'), false},
        {"qUncompress fails", QByteArray("\x00\x00\x00\x10", 4) + QByteArray(64, 'x'), false},
    };
    auto add = [&](const char *name, const Stream &s) { cases.push_back({name, s.build(), false}); };
    { Stream s; s.magic = 0x12345678; add("wrong magic", s); }
    { Stream s; s.version = 4; add("newer version", s); }
    { Stream s; s.rows = 0; add("zero rows", s); }
    { Stream s; s.rows = 2001; s.writeLines = false; add("rows past cap", s); }
    { Stream s; s.cols = 4001; s.writeLines = false; add("cols past cap", s); }
    { Stream s; s.sbLines = -1; s.writeLines = false; add("negative scrollback", s); }
    { Stream s; s.sbLines = 1000001; s.writeLines = false; add("scrollback past cap", s); }
    { Stream s; s.sbCells = 10001; add("line cells past cap", s); }
    { Stream s; s.combEntries = 10001; add("combining entries past cap", s); }
    { Stream s; s.marks = 9; add("marks past cap", s); }
    { Stream s; s.screenRows = 2001; s.writeLines = false; add("screen rows past cap", s); }
    // Header 24 bytes, scrollback count 4, cell count 4, wrapped 1, then cells.
    { Stream s; s.truncateTo = 24 + 4 + 4 + 1 + 6; add("cut mid-cell", s); }
    // ...three 13-byte cells, combining count 4, column and mark count 8.
    { Stream s; s.truncateTo = 24 + 4 + 4 + 1 + 13 * 3 + 4 + 8 + 2; add("cut mid-mark", s); }
    { Stream s; s.withCwd = false; s.withPin = false; s.title.clear();
      QByteArray raw = qUncompress(s.build()); raw.chop(2);
      cases.push_back({"cut in title", qCompress(raw, 6), false}); }

    for (const Case &c : cases) {
        const Read r = viaRestore(c.data);
        const Read p = viaPeek(c.data);
        EXPECT_EQ(r.ok, c.accepted) << c.name << ": the corpus case is not what it claims";
        EXPECT_EQ(p.ok, r.ok) << c.name << ": peek and restore disagree on acceptance";
        if (!r.ok || !p.ok) continue;
        EXPECT_EQ(p.title, r.title) << c.name;
        EXPECT_EQ(p.cwd, r.cwd) << c.name;
        EXPECT_EQ(p.pin, r.pin) << c.name;
    }
    const Read realPeek = viaPeek(real);
    EXPECT_EQ(realPeek.title, QStringLiteral("saved-title"));
    EXPECT_EQ(realPeek.cwd, QStringLiteral("/saved/cwd"));
    EXPECT_EQ(realPeek.pin, QStringLiteral("saved-pin"));
    EXPECT_TRUE(viaPeek(v2.build()).pin.isEmpty()) << "a V2 stream has no pin";
}

// INV-1 through the file readers the startup path uses.
TEST(LazyTabRestore, Inv1PeekSessionMatchesLoadSession) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ants_test::XdgGuard xdg;
    xdg.setTestMode(false);
    xdg.setEnv("XDG_DATA_HOME", tmp.path().toUtf8());

    const QString id = QStringLiteral("ants-5225-peek");
    TerminalGrid source(6, 20);
    feed(source, "\x1b]0;file-title\x07hello\r\n");
    SessionManager::saveSession(id, &source, QStringLiteral("/file/cwd"),
                                QStringLiteral("file-pin"));

    SessionPeek peeked;
    ASSERT_TRUE(SessionManager::peekSession(id, &peeked));
    TerminalGrid target(24, 80);
    QString cwd, pin;
    ASSERT_TRUE(SessionManager::loadSession(id, &target, &cwd, &pin));
    EXPECT_EQ(peeked.title, target.windowTitle());
    EXPECT_EQ(peeked.cwd, cwd);
    EXPECT_EQ(peeked.pinnedTitle, pin);
    EXPECT_EQ(peeked.pinnedTitle, QStringLiteral("file-pin"));
}

// INV-6 — a file tab_order.txt names never ages out; an unlisted one does.
TEST(LazyTabRestore, Inv6ListedFilesNeverAgeOut) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ants_test::XdgGuard xdg;
    xdg.setTestMode(false);
    xdg.setEnv("XDG_DATA_HOME", tmp.path().toUtf8());

    const QString listed = SessionManager::sessionPath(QStringLiteral("ants-5225-listed"));
    const QString unlisted = SessionManager::sessionPath(QStringLiteral("ants-5225-unlisted"));
    for (const QString &p : {listed, unlisted}) {
        QFile f(p);
        ASSERT_TRUE(f.open(QIODevice::ReadWrite)) << qPrintable(p);
        f.write("blob");
        ASSERT_TRUE(f.flush());  // a write flushed at close would reset the mtime
        ASSERT_TRUE(f.setFileTime(QDateTime::currentDateTime().addDays(-40),
                                  QFileDevice::FileModificationTime));
    }
    SessionManager::saveTabOrder({QStringLiteral("ants-5225-listed")}, 0);

    SessionManager::cleanupOldSessions(30);
    EXPECT_TRUE(QFile::exists(listed)) << "a file tab_order.txt names was aged out";
    EXPECT_FALSE(QFile::exists(unlisted)) << "an unlisted 40-day-old file was kept";
}

// INV-3 — startup peeks; only wakeRestoredTab loads history and starts shells.
TEST(LazyTabRestore, Inv3RestoreLoadsNothingItself) {
    const std::string src = mainWindow();
    const std::string restore = body(src, "void MainWindow::restoreSessions(");
    ASSERT_FALSE(restore.empty());
    EXPECT_NE(restore.find("peekSession("), std::string::npos);
    EXPECT_NE(restore.find("wakeRestoredTab("), std::string::npos);
    EXPECT_EQ(restore.find("loadSession("), std::string::npos)
        << "restoreSessions loads a tab's history itself";
    EXPECT_EQ(restore.find("startShell("), std::string::npos)
        << "restoreSessions starts a shell itself";

    const std::string wake = body(src, "void MainWindow::wakeRestoredTab(");
    ASSERT_FALSE(wake.empty());
    const size_t load = wake.find("loadSession(");
    const size_t start = wake.find("startShell(");
    const size_t track = wake.find("trackTerminalShell(");
    const size_t pid = wake.find("setShellPid(");
    ASSERT_NE(load, std::string::npos);
    ASSERT_NE(start, std::string::npos);
    ASSERT_NE(track, std::string::npos);
    ASSERT_NE(pid, std::string::npos);
    EXPECT_LT(load, start);
    EXPECT_LT(start, track);
    EXPECT_LT(track, pid);
}

// INV-4 — a tab wakes when it becomes current, is renamed, or is named by
// index in a remote-control command.
TEST(LazyTabRestore, Inv4EveryNamedRouteWakes) {
    const std::string src = mainWindow();
    const std::string changed = body(src, "void MainWindow::onTabChanged(");
    ASSERT_FALSE(changed.empty());
    const size_t wake = changed.find("wakeRestoredTab(");
    ASSERT_NE(wake, std::string::npos);
    EXPECT_LT(wake, changed.find("focusedTerminal()"));

    const std::string rename = body(src, "bool MainWindow::setTabTitleForRemote(");
    ASSERT_FALSE(rename.empty());
    EXPECT_NE(rename.find("wakeRestoredTab("), std::string::npos);

    const std::string rc = ants_test::stripComments(ants_test::slurpRemoteControl());
    ASSERT_FALSE(rc.empty());
    EXPECT_NE(rc.find("wakeTerminalAtTab("), std::string::npos);
    EXPECT_EQ(rc.find("terminalAtTab("), std::string::npos)
        << "a RemoteControl command resolves a tab without waking it";
}

// INV-5 — a sleeping tab keeps its place in tab_order.txt and gets no blob.
TEST(LazyTabRestore, Inv5SaveSkipsSleepingTabsAfterOrdering) {
    const std::string save = body(mainWindow(), "void MainWindow::saveAllSessions(");
    ASSERT_FALSE(save.empty());
    const size_t ordered = save.find("tabOrder.append(");
    const size_t skipped = save.find("m_sleepingTabs");
    const size_t written = save.find("SessionManager::saveSession(");
    ASSERT_NE(ordered, std::string::npos);
    ASSERT_NE(skipped, std::string::npos);
    ASSERT_NE(written, std::string::npos);
    EXPECT_LT(ordered, skipped);
    EXPECT_LT(skipped, written);
}

// INV-7 — the 2 s title refresh leaves a sleeping tab's saved label alone.
TEST(LazyTabRestore, Inv7TitleRefreshSkipsSleepingTabs) {
    const std::string titles = body(mainWindow(), "void MainWindow::updateTabTitles(");
    ASSERT_FALSE(titles.empty());
    EXPECT_NE(titles.find("m_sleepingTabs"), std::string::npos);
}
