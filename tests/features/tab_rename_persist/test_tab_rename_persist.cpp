// Feature-conformance test for spec.md —
//
// I1: sessionmanager.h VERSION == 3.
// I2: serialize(grid, cwd, pinned) + restore round-trips pinned.
// I3: V2 stream (no pinnedTitle) loads, leaves out-param empty.
// I4: mainwindow.cpp saveAllSessions loop passes m_tabTitlePins.value(w)
//     as the 4th arg of SessionManager::saveSession.
// I5: mainwindow.cpp restoreSessions calls loadSession(..., &savedPinnedTitle)
//     and populates m_tabTitlePins[terminal] from savedPinnedTitle.
//
// Exit 0 = all invariants hold. Non-zero = regression.

#include "sessionmanager.h"
#include "terminalgrid.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDataStream>
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

QString slurp(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::fprintf(stderr, "cannot open %s\n", qUtf8Printable(path));
        std::exit(2);
    }
    return QString::fromUtf8(f.readAll());
}

// I1: VERSION constant bumped.
void checkI1() {
    const QString hdr = slurp(QStringLiteral(SRC_SESSIONMANAGER_H_PATH));
    expect(hdr.contains(QStringLiteral("VERSION = 3")),
           "I1/schema-version-3",
           QStringLiteral("sessionmanager.h no longer declares "
                          "`VERSION = 3` — schema bump lost?"));
}

// I2: round-trip pinnedTitle (non-empty, empty, long) via serialize/restore.
void checkI2() {
    for (const QString &pin : {
             QStringLiteral("Deploy"),
             QStringLiteral(""),
             QStringLiteral("this is a 64-char tab rename to confirm "
                            "no silent truncat"),
         }) {
        TerminalGrid grid(24, 80);
        const QString cwd = QStringLiteral("/home/user/src");

        const QByteArray payload =
            SessionManager::serialize(&grid, cwd, pin);
        expect(!payload.isEmpty(), "I2/serialize-produces-output");

        TerminalGrid restored(24, 80);
        QString cwdOut;
        QString pinOut;
        const bool ok = SessionManager::restore(&restored, payload,
                                                &cwdOut, &pinOut);
        expect(ok, "I2/restore-returns-true");
        expect(cwdOut == cwd, "I2/cwd-round-trip",
               QStringLiteral("cwd round-trip wrong: got '%1' expected '%2'")
                   .arg(cwdOut, cwd));
        expect(pinOut == pin,
               "I2/pinnedTitle-round-trip",
               QStringLiteral("pinnedTitle round-trip wrong: got '%1' "
                              "expected '%2' (length %3 → %4)")
                   .arg(pinOut, pin)
                   .arg(pin.size())
                   .arg(pinOut.size()));
    }
}

// I3: V2-era file (no pinnedTitle field) loads, leaves out-param empty.
// Hand-craft a minimal V2 stream so we don't depend on a 0.7.16-tarball
// fixture lying in tree.
void checkI3() {
    constexpr uint32_t MAGIC = 0x414E5453;  // "ANTS"

    QByteArray raw;
    {
        QDataStream out(&raw, QIODevice::WriteOnly);
        out.setVersion(QDataStream::Qt_6_0);
        out << MAGIC << uint32_t{2};                    // V2 header
        out << int32_t{24} << int32_t{80};              // rows, cols
        out << int32_t{0} << int32_t{0};                // cursor
        out << int32_t{0};                              // scrollback count
        out << int32_t{24};                             // screen rows
        for (int r = 0; r < 24; ++r) {
            out << int32_t{80};                         // col count
            for (int c = 0; c < 80; ++c) {
                out << uint32_t{' '};                   // codepoint
                out << QColor(Qt::white).rgba();        // fg
                out << QColor(Qt::black).rgba();        // bg
                out << uint8_t{0};                      // flags
            }
            out << int32_t{0};                          // combining count
        }
        out << QString();                               // window title
        out << QStringLiteral("/home/test");            // V2 cwd
        // Deliberately no V3 pinnedTitle — simulating an old file.
    }
    const QByteArray compressed = qCompress(raw, 6);

    TerminalGrid restored(24, 80);
    QString cwdOut;
    QString pinOut = QStringLiteral("POISON");  // must be cleared to empty
    const bool ok = SessionManager::restore(&restored, compressed,
                                            &cwdOut, &pinOut);
    expect(ok, "I3/v2-file-loads-successfully",
           QStringLiteral("V2 back-compat: restore returned false"));
    expect(cwdOut == QStringLiteral("/home/test"),
           "I3/v2-cwd-still-works",
           QStringLiteral("V2 cwd field not preserved: got '%1'")
               .arg(cwdOut));
    expect(pinOut.isEmpty(),
           "I3/v2-pinnedTitle-out-param-empty",
           QStringLiteral("V2 file → pinnedTitle out-param should be "
                          "empty, got '%1' (restore must not leave the "
                          "POISON sentinel if the field is absent)")
               .arg(pinOut));
}

// Production-binding invariants — grep mainwindow.cpp.
void checkI4I5() {
    const QString src = slurp(QStringLiteral(SRC_MAINWINDOW_CPP_PATH));

    // I4: saveSession call in the save loop passes a pin lookup.
    expect(src.contains(QStringLiteral(
               "SessionManager::saveSession(tabId, t->grid(), t->shellCwd(), pinnedTitle)")),
           "I4/saveSession-threads-pinnedTitle",
           QStringLiteral("mainwindow.cpp saveAllSessions loop no longer "
                          "passes a pinnedTitle to SessionManager::"
                          "saveSession — rename won't survive restart"));

    // The pin lookup itself must resolve from m_tabTitlePins.value(w).
    expect(src.contains(QStringLiteral("m_tabTitlePins.value(w)")),
           "I4/pin-lookup-keyed-by-outer-tab-widget",
           QStringLiteral("pinnedTitle save-side lookup must use "
                          "m_tabTitlePins.value(w) — the outer tab "
                          "widget, which may be a QSplitter for split "
                          "tabs"));

    // I5: restore path reads pinnedTitle and writes it back into the map.
    expect(src.contains(QStringLiteral(
               "SessionManager::loadSession(tabId, terminal->grid(), &savedCwd,"))
           && src.contains(QStringLiteral("&savedPinnedTitle")),
           "I5/loadSession-reads-pinnedTitle",
           QStringLiteral("mainwindow.cpp restoreSessions no longer "
                          "asks loadSession for the pinnedTitle out-"
                          "param — pin survives on disk but never reloaded"));
    expect(src.contains(QStringLiteral(
               "m_tabTitlePins[terminal] = savedPinnedTitle")),
           "I5/pin-map-populated-from-restore",
           QStringLiteral("m_tabTitlePins[terminal] = savedPinnedTitle "
                          "missing from restoreSessions — without it, "
                          "next titleChanged re-labels the tab"));
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    checkI1();
    checkI2();
    checkI3();
    checkI4I5();

    if (g_failures) {
        std::fprintf(stderr, "\n%d invariant(s) failed\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nall invariants hold\n");
    return 0;
}
