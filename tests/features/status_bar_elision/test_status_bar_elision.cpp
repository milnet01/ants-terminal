// Feature-conformance test for spec.md — asserts ElidedLabel's elision
// policy:
//   1. Short text (fits in maximumWidth) never elides.
//   2. Over-cap text elides to the cap.
//   3. minimumSizeHint respects the full-text width (no squeeze-to-"…").
//   4. Tooltip reflects elision state.
//
// ElidedLabel is header-only, so this test pulls it in directly — no
// link against src/*.cpp needed. Uses QApplication (not QCoreApplication)
// because QLabel::fontMetrics and sizeHint require a QGuiApplication
// to be alive.
//
// Exit 0 = all assertions hold. Non-zero = regression.

#include "elidedlabel.h"
#include "../../_support/srcgrep.h"

#include <QApplication>
#include <QStatusBar>
#include <QMainWindow>
#include <QFontMetrics>
#include <QSizePolicy>
#include <QHash>
#include <QStringList>
#include <QRegularExpression>
#include <QLayout>

#include <cstdio>
#include <string>
#include <gtest/gtest.h>

namespace {

// CHECK previously appended to a module-level `int failures` counter that
// no TEST() body ever asserted on — so condition failures printed to
// stderr but gtest reported PASSED. Route through ADD_FAILURE_AT instead
// so a CHECK miss is a real gtest failure with the source location.
#define CHECK(cond, msg) do {                                                \
    if (!(cond)) {                                                           \
        ADD_FAILURE_AT(__FILE__, __LINE__) << msg;                          \
    }                                                                        \
} while (0)

// Invariant 1 + 3: short text in a capped label must render in full and
// its minimumSizeHint must allow the full-text render.
TEST(StatusBarElision, ShortTextUnderCap) {
    ElidedLabel lbl;
    lbl.setMaximumWidth(220);
    lbl.setElideMode(Qt::ElideRight);
    lbl.setFullText(" main");

    const QFontMetrics fm(lbl.font());
    const int textW = fm.horizontalAdvance(" main");

    // text() must equal fullText — no elision on short text.
    CHECK(lbl.text() == QString(" main"),
          "short text elided when it should fit");

    // minimumSizeHint width must be >= textW — preventing the layout
    // from squeezing the widget below what the text needs.
    CHECK(lbl.minimumSizeHint().width() >= textW,
          "minimumSizeHint < full-text width (layout will squeeze → '…')");

    // Minimum must not exceed cap (220). The full-text width for " main"
    // at default font is well under 220, so this is also textW.
    CHECK(lbl.minimumSizeHint().width() <= 220,
          "minimumSizeHint > maximumWidth (widget demands more than cap)");

    // Tooltip must be empty when no elision happened.
    CHECK(lbl.toolTip().isEmpty(),
          "tooltip set when no elision occurred");
}

// Invariant 2 + 4: over-cap text must elide, tooltip must carry the
// full string for hover.
TEST(StatusBarElision, LongTextOverCap) {
    ElidedLabel lbl;
    lbl.setMaximumWidth(60);      // deliberately tiny cap
    lbl.setElideMode(Qt::ElideRight);
    lbl.resize(60, 20);            // simulate layout assigning the cap width
    const QString longText = " feature/very-long-branch-name-indeed";
    lbl.setFullText(longText);

    // Displayed text must differ from full text — elision occurred.
    CHECK(lbl.text() != longText,
          "over-cap text NOT elided (cap ignored)");

    // Displayed text must contain the elision mark "…".
    CHECK(lbl.text().contains(QChar(0x2026)),
          "elided text missing '…' mark");

    // Tooltip must be the full text.
    CHECK(lbl.toolTip() == longText,
          "tooltip does not carry full text after elision");

    // minimumSizeHint must be capped at maximumWidth — not the full text's
    // width (which would force the widget to demand far more space than
    // its cap allows).
    CHECK(lbl.minimumSizeHint().width() <= 60,
          "minimumSizeHint exceeds maximumWidth for over-cap text");
}

// Invariant 3 extension: in a real QStatusBar, addWidget() layout must
// respect the minimumSizeHint so short text survives tight-space
// squeeze. This exercises the actual regression vector.
TEST(StatusBarElision, StatusBarLayoutDoesNotSqueeze) {
    QMainWindow win;
    QStatusBar *bar = win.statusBar();

    auto *branch = new ElidedLabel(&win);
    branch->setMaximumWidth(220);
    branch->setElideMode(Qt::ElideRight);
    bar->addWidget(branch);

    auto *message = new ElidedLabel(&win);
    message->setElideMode(Qt::ElideMiddle);
    bar->addWidget(message, /*stretch=*/1);

    auto *proc = new ElidedLabel(&win);
    proc->setMaximumWidth(160);
    proc->setElideMode(Qt::ElideRight);
    bar->addWidget(proc);

    // Seed typical content: short branch, long message, short process.
    branch->setFullText(" main");
    message->setFullText(
        "Claude permission: Bash(git log --format=%H -- src/mainwindow.cpp)");
    proc->setFullText("bash");

    // Resize narrowly to induce layout pressure (narrow enough to force
    // the stretch-1 middle slot to elide, wide enough that branch+proc
    // should still show in full).
    win.resize(520, 120);
    win.show();
    QApplication::processEvents();

    CHECK(branch->text() == QString(" main"),
          "branch chip elided to '…' under tight layout (regression)");

    CHECK(proc->text() == QString("bash"),
          "process chip elided to '…' under tight layout (regression)");

    // Middle slot is expected to elide since long text + narrow window.
    // Just assert tooltip is set (elision must have a recoverable tooltip).
    const bool middleElided = (message->text() != message->fullText());
    CHECK(!middleElided || !message->toolTip().isEmpty(),
          "middle slot elided but tooltip missing full text");
}

// ---------------------------------------------------------------------
// Invariant 6 — an uncapped status-message slot must not raise the window's
// minimum width.
//
// Why this exists: 2026-09-29 the main window widened itself to a very wide,
// short shape because the uncapped status-message ElidedLabel reported the
// full text's width as its minimumSizeHint, so a long message raised the
// QStatusBar's and the window's minimum width instead of eliding.
//
// Fidelity choice: nothing in the suite constructs a real MainWindow (it
// spawns PTYs, tabs, timers and config), so the slot is REBUILT here from
// the statements MainWindow's constructor applies to it. The rebuild is tied
// to src/mainwindow*.cpp: the test parses the real construction block
// (`new ElidedLabel(this)` .. `m_statusMessage = lbl;` and the
// `addWidget(m_statusMessage, N)` call), replays every setter it finds, and
// FAILS on a statement it cannot replay. So a fix made in MainWindow (a
// setter on the slot) is replayed, and a divergence cannot keep this green.

struct SlotSpec {
    bool found = false;
    QString problem;                       // non-empty => cannot replay
    Qt::TextElideMode mode = Qt::ElideRight;
    int maxW = -1, minW = -1;              // -1 = not set
    bool hasPolicy = false;
    QSizePolicy::Policy polH = QSizePolicy::Preferred;
    QSizePolicy::Policy polV = QSizePolicy::Preferred;
    int stretch = 0;
};

QSizePolicy::Policy parsePolicy(const QString &n, bool *ok) {
    static const QHash<QString, QSizePolicy::Policy> m = {
        {"Fixed", QSizePolicy::Fixed}, {"Minimum", QSizePolicy::Minimum},
        {"Maximum", QSizePolicy::Maximum}, {"Preferred", QSizePolicy::Preferred},
        {"Expanding", QSizePolicy::Expanding},
        {"MinimumExpanding", QSizePolicy::MinimumExpanding},
        {"Ignored", QSizePolicy::Ignored}};
    *ok = m.contains(n);
    return m.value(n, QSizePolicy::Preferred);
}

SlotSpec parseRealSlot() {
    SlotSpec sp;
    const QString src = QString::fromStdString(ants_test::slurpMainWindow());
    const int end = src.indexOf(QStringLiteral("m_statusMessage = lbl;"));
    if (end < 0) { sp.problem = "anchor 'm_statusMessage = lbl;' not found in MainWindow sources"; return sp; }
    const int begin = src.lastIndexOf(QStringLiteral("new ElidedLabel(this)"), end);
    if (begin < 0) { sp.problem = "'new ElidedLabel(this)' not found before the anchor"; return sp; }
    sp.found = true;
    QString block = src.mid(begin, end - begin);
    block.remove(QRegularExpression(QStringLiteral("//[^\n]*")));
    static const QRegularExpression stmt(QStringLiteral("lbl->(\\w+)\\(([^;]*)\\);"));
    auto it = stmt.globalMatch(block);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString fn = m.captured(1), args = m.captured(2).trimmed();
        if (fn == "setElideMode") {
            if (args.endsWith("ElideMiddle")) sp.mode = Qt::ElideMiddle;
            else if (args.endsWith("ElideLeft")) sp.mode = Qt::ElideLeft;
            else if (args.endsWith("ElideRight")) sp.mode = Qt::ElideRight;
            else sp.problem = "unreplayable setElideMode(" + args + ")";
        } else if (fn == "setMaximumWidth" || fn == "setMinimumWidth") {
            bool ok = false; const int v = args.toInt(&ok);
            if (!ok) { sp.problem = "unreplayable " + fn + "(" + args + ") - use an integer literal or extend the test"; continue; }
            (fn == "setMaximumWidth" ? sp.maxW : sp.minW) = v;
        } else if (fn == "setSizePolicy") {
            const QStringList a = args.split(',');
            bool ok1 = false, ok2 = false;
            if (a.size() == 2) {
                sp.polH = parsePolicy(a[0].trimmed().section("::", -1), &ok1);
                sp.polV = parsePolicy(a[1].trimmed().section("::", -1), &ok2);
            }
            if (!ok1 || !ok2) sp.problem = "unreplayable setSizePolicy(" + args + ")";
            else sp.hasPolicy = true;
        } else {
            sp.problem = "unreplayable statement lbl->" + fn + "(" + args + ") - extend the test's replay";
        }
    }
    static const QRegularExpression add(
        QStringLiteral("addWidget\\(\\s*m_statusMessage\\s*,\\s*(\\d+)\\s*\\)"));
    const auto am = add.match(src);
    if (!am.hasMatch()) sp.problem = "addWidget(m_statusMessage, <stretch>) not found";
    else sp.stretch = am.captured(1).toInt();
    return sp;
}

QString messageOfWidth(const QFontMetrics &fm, int px) {
    QString s = QStringLiteral("START-");
    while (fm.horizontalAdvance(s) < px) s += QStringLiteral("status message word ");
    return s + QStringLiteral("-END");
}

// Builds the window with the branch chip, the message slot and the process
// chip in MainWindow's order, sets `message`, and returns the results.
struct Probe {
    int minW = 0;            // window minimumSizeHint().width()
    int barMinW = 0;         // status bar minimumSizeHint().width()
    int winW = 0;            // window width after layout at 800 px
    QString shown, tip, branchShown;
};

Probe probe(const SlotSpec &sp, const QString &message) {
    Probe r;
    QMainWindow win;
    QStatusBar *bar = win.statusBar();
    auto *branch = new ElidedLabel(&win);
    branch->setMaximumWidth(220);
    branch->setElideMode(Qt::ElideRight);
    bar->addWidget(branch);

    auto *lbl = new ElidedLabel(&win);
    lbl->setElideMode(sp.mode);
    if (sp.maxW >= 0) lbl->setMaximumWidth(sp.maxW);
    if (sp.minW >= 0) lbl->setMinimumWidth(sp.minW);
    if (sp.hasPolicy) lbl->setSizePolicy(sp.polH, sp.polV);
    bar->addWidget(lbl, sp.stretch);

    auto *proc = new QLabel(&win);
    bar->addWidget(proc);

    branch->setFullText(" main");
    proc->setText("bash");
    lbl->setFullText(message);

    win.resize(800, 300);
    win.show();
    QApplication::processEvents();
    if (win.layout()) win.layout()->activate();
    r.minW = win.minimumSizeHint().width();
    r.barMinW = bar->minimumSizeHint().width();
    r.winW = win.width();
    r.shown = lbl->text();
    r.tip = lbl->toolTip();
    r.branchShown = branch->text();
    return r;
}

TEST(StatusBarElision, UncappedMessageDoesNotWidenWindow) {
    const SlotSpec sp = parseRealSlot();
    ASSERT_TRUE(sp.found) << sp.problem.toStdString();
    ASSERT_TRUE(sp.problem.isEmpty())
        << "the test cannot replay MainWindow's status-message slot: " << sp.problem.toStdString();

    const QFontMetrics fm{QFont()};
    const QString shortMsg = QStringLiteral("Saved");
    const QString msg3k = messageOfWidth(fm, 3000);
    const QString msg6k = messageOfWidth(fm, 6000);
    const int msgW = fm.horizontalAdvance(msg3k);
    ASSERT_GE(msgW, 3000);

    const Probe base = probe(sp, shortMsg);
    const Probe p3 = probe(sp, msg3k);
    const Probe p6 = probe(sp, msg6k);

    // INV-6a: the window's minimum width does not grow with the message.
    // Expected: well under the message's own width, and identical for a
    // message twice as long.
    EXPECT_LT(p3.minW, msgW / 2)
        << "expected window minimumSizeHint().width() well under the " << msgW
        << " px message width; actual " << p3.minW
        << " (short-message baseline " << base.minW << ")";
    EXPECT_LT(p3.barMinW, msgW / 2)
        << "expected status bar minimumSizeHint().width() < " << msgW / 2
        << "; actual " << p3.barMinW << " (baseline " << base.barMinW << ")";
    EXPECT_EQ(p3.minW, p6.minW)
        << "window minimum width scales with message length: 3000 px message -> "
        << p3.minW << ", 6000 px message -> " << p6.minW;

    // INV-6b: the window did not widen past the width it was given.
    EXPECT_EQ(p3.winW, 800)
        << "expected the window to stay 800 px wide; actual " << p3.winW;

    // INV-6c: the slot elides, and is not merely hidden.
    EXPECT_NE(p3.shown.toStdString(), msg3k.toStdString()) << "long message displayed in full at 800 px (not elided)";
    EXPECT_TRUE(p3.shown.contains(QChar(0x2026)))
        << "elided text lacks the '…' mark; actual: " << p3.shown.toStdString();
    EXPECT_TRUE(p3.shown.startsWith(QStringLiteral("START")) && p3.shown.endsWith(QStringLiteral("-END")))
        << "ElideMiddle must keep both ends visible; actual: " << p3.shown.toStdString();
    EXPECT_EQ(p3.tip.toStdString(), msg3k.toStdString()) << "tooltip must carry the full message after elision";

    // Positive control: the capped branch chip is not squeezed by the long message.
    EXPECT_EQ(p3.branchShown.toStdString(), std::string(" main"))
        << "branch chip squeezed while the message slot is long; actual: "
        << p3.branchShown.toStdString();
}


}  // namespace

