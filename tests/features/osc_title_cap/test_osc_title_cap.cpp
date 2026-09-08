// Why this exists: the OSC 0/2 window title was the one attacker-supplied
// OSC payload in handleOsc with no length bound, and it is persisted into
// the session blob and restored on the next launch. See spec.md.
//
// Drives TerminalGrid through processAction with a VtAction::OscEnd
// payload, the same way osc_color_query does, and calls setTitle directly
// for the session-restore path.

#include "terminalgrid.h"
#include "vtparser.h"

#include <gtest/gtest.h>
#include <string>

namespace {

constexpr int kRows = 24;
constexpr int kCols = 80;

// The contract's bound. Deliberately spelled out here rather than read
// from the header: a test that imports the constant it is checking moves
// with the code and pins nothing.
constexpr int kBound = 1024;

struct Probe {
    TerminalGrid grid;
    Probe() : grid(kRows, kCols) {}

    void osc(const std::string &payload) {
        VtAction a;
        a.type = VtAction::OscEnd;
        a.oscString = payload;
        grid.processAction(a);
    }
};

}  // namespace

TEST(OscTitleCap, Inv1OversizeOsc2IsTruncated) {
    Probe p;
    const std::string huge(64 * 1024, 'A');
    p.osc("2;" + huge);
    EXPECT_LE(p.grid.windowTitle().size(), kBound)
        << "an OSC 2 title rides the parser's multi-megabyte OSC "
           "accumulator; unbounded it reaches the window manager and is "
           "written into the session blob, so it comes back at every launch";
    EXPECT_GT(p.grid.windowTitle().size(), 0)
        << "the title must be truncated, not discarded";
}

TEST(OscTitleCap, Inv2OrdinaryTitleIsUntouched) {
    Probe p;
    const QString ordinary =
        QStringLiteral("ants@host: ~/src/ants-terminal — vim terminalgrid.cpp");
    p.osc("2;" + ordinary.toStdString());
    EXPECT_EQ(p.grid.windowTitle(), ordinary)
        << "the cap must not disturb a normal title";
}

TEST(OscTitleCap, Inv3Osc0IsBoundedToo) {
    Probe p;
    const std::string huge(64 * 1024, 'B');
    p.osc("0;" + huge);
    EXPECT_LE(p.grid.windowTitle().size(), kBound)
        << "OSC 0 and OSC 2 both set the title and must share the bound";
}

TEST(OscTitleCap, Inv4RestorePathIsBounded) {
    Probe p;
    // The shape SessionManager produces when it reads a blob written
    // before this fix: no OSC involved, straight into setTitle.
    p.grid.setTitle(QString(64 * 1024, QLatin1Char('C')));
    EXPECT_LE(p.grid.windowTitle().size(), kBound)
        << "capping only the OSC ingress leaves an old session blob free "
           "to reintroduce a title no OSC could set any more";
}

TEST(OscTitleCap, Inv5TruncationLeavesNoLoneSurrogate) {
    Probe p;
    // U+1F600 is one astral character = two UTF-16 code units. An odd
    // prefix before it puts the bound inside a surrogate pair.
    QString s = QStringLiteral("x");
    while (s.size() < 4096) s += QString::fromUcs4(U"\U0001F600", 1);
    p.grid.setTitle(s);

    const QString got = p.grid.windowTitle();
    EXPECT_LE(got.size(), kBound);
    ASSERT_FALSE(got.isEmpty());
    EXPECT_FALSE(got.back().isHighSurrogate())
        << "truncation split a surrogate pair and left half a character; "
           "that unpaired code unit is handed to the window manager";
}
