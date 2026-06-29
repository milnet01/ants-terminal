// ANTS-1078 — terminal screen-reader accessibility conformance test.
// Contract: tests/features/terminal_a11y/spec.md. Design + invariants:
// docs/specs/ANTS-1078.md. Runs in the test_chrome GUI bundle (constructing
// a full TerminalWidget pulls remotecontrol -> MainWindow, whose symbols live
// in ants_chrome_lib) under QT_QPA_PLATFORM=offscreen.

#include <gtest/gtest.h>

#include <QAccessible>
#include <QFile>
#include <QMetaObject>
#include <QPoint>
#include <QRect>
#include <QString>

#include "terminalaccessible.h"
#include "terminalgrid.h"
#include "terminalwidget.h"
#include "vtparser.h"

namespace {

// Construct a TerminalWidget (no shell — startShell() is never called),
// size its grid, and feed it bytes through a VtParser the way the live
// data path does (PTY -> VtParser -> TerminalGrid).
struct Harness {
    TerminalWidget w;
    VtParser parser{[this](const VtAction &a) { w.grid()->processAction(a); }};
    explicit Harness(int rows = 4, int cols = 12) { w.grid()->resize(rows, cols); }
    void feed(const std::string &s) { parser.feed(s.data(), static_cast<int>(s.size())); }
};

// INV-9 spy state (the update handler is a process-global function ptr).
int g_evtCount = 0;
int g_lastCursorPos = -1;
void spyHandler(QAccessibleEvent *ev) {
    if (ev->type() == QAccessible::TextCaretMoved) {
        ++g_evtCount;
        g_lastCursorPos = static_cast<QAccessibleTextCursorEvent *>(ev)->cursorPosition();
    }
}

} // namespace

// INV-2 / INV-3 — viewport text, character count, slicing.
TEST(TerminalA11y, ViewportTextAndSlicing) {
    Harness h;
    h.feed("abc\r\ndef");
    EXPECT_EQ(h.w.accessibleText(), QStringLiteral("abc\ndef"));   // INV-2: trailing rows dropped

    installTerminalAccessibilityFactory();
    QAccessible::setActive(true);
    QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(&h.w);
    ASSERT_NE(iface, nullptr);
    QAccessibleTextInterface *ti = iface->textInterface();
    ASSERT_NE(ti, nullptr);
    EXPECT_EQ(ti->characterCount(), 7);                            // INV-3
    EXPECT_EQ(ti->text(0, 7), QStringLiteral("abc\ndef"));
    EXPECT_EQ(ti->text(0, 3), QStringLiteral("abc"));
    EXPECT_EQ(ti->text(0, -1), QStringLiteral("abc\ndef"));        // -1 => to end
}

// INV-2 — an all-blank viewport is the empty string.
TEST(TerminalA11y, EmptyViewport) {
    Harness h;   // nothing fed
    EXPECT_EQ(h.w.accessibleText(), QString());
    EXPECT_EQ(h.w.accessibleText().size(), 0);
    EXPECT_EQ(h.w.accessibleCaretOffset(), 0);                     // INV-4: blank screen -> 0
}

// INV-1 — the queried interface is a Terminal with a text interface.
TEST(TerminalA11y, InterfaceRoleAndTextInterface) {
    Harness h;
    h.feed("hello");
    installTerminalAccessibilityFactory();
    QAccessible::setActive(true);
    QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(&h.w);
    ASSERT_NE(iface, nullptr);
    EXPECT_EQ(iface->role(), QAccessible::Terminal);
    ASSERT_NE(iface->textInterface(), nullptr);
    EXPECT_EQ(iface->textInterface()->text(0, iface->textInterface()->characterCount()),
              h.w.accessibleText());
}

// INV-4 — caret offset tracks the cursor within the viewport text.
TEST(TerminalA11y, CaretOffset) {
    Harness h;
    h.feed("abc\r\ndef");           // cursor ends at row 1, col 3
    EXPECT_EQ(h.w.accessibleCaretOffset(), 7);   // "abc"(3) + '\n'(1) + "def"(3)
}

// INV-5 — word/line boundary navigation stays in range; word is correct.
TEST(TerminalA11y, BoundaryNavigation) {
    Harness h;
    h.feed("foo bar\r\nbaz");
    installTerminalAccessibilityFactory();
    QAccessible::setActive(true);
    QAccessibleTextInterface *ti = QAccessible::queryAccessibleInterface(&h.w)->textInterface();
    ASSERT_NE(ti, nullptr);
    const int n = ti->characterCount();

    int s = -1, e = -1;
    const QString word = ti->textAtOffset(0, QAccessible::WordBoundary, &s, &e);
    EXPECT_EQ(word, QStringLiteral("foo"));
    EXPECT_EQ(s, 0);
    EXPECT_EQ(e, 3);

    for (int off = 0; off <= n; ++off) {
        for (auto b : {QAccessible::WordBoundary, QAccessible::LineBoundary}) {
            int bs = -1, be = -1;
            ti->textAtOffset(off, b, &bs, &be);
            EXPECT_GE(bs, 0);
            EXPECT_LE(be, n);
            EXPECT_LE(bs, be);
        }
    }
}

// INV-6 — rect/offset round-trip on single-column ASCII + the -1 cases.
TEST(TerminalA11y, RectOffsetRoundTrip) {
    Harness h;
    h.feed("abcde");                          // one row of single-column ASCII
    const QRect r0 = h.w.accessibleRectForOffset(0);
    ASSERT_FALSE(r0.isNull());
    const int pad = r0.x(), cw = r0.width(), ch = r0.height();
    ASSERT_GT(cw, 0);
    ASSERT_GT(ch, 0);

    for (int o = 0; o < 5; ++o) {
        const QRect r = h.w.accessibleRectForOffset(o);
        ASSERT_FALSE(r.isNull()) << "offset " << o;
        EXPECT_EQ(r.height(), ch);
        EXPECT_EQ(h.w.accessibleOffsetAt(r.center()), o) << "round-trip offset " << o;
    }

    // -1 cases.
    EXPECT_EQ(h.w.accessibleOffsetAt(QPoint(pad - 1, pad - 1)), -1);          // padding
    EXPECT_EQ(h.w.accessibleOffsetAt(QPoint(pad + 8 * cw + cw / 2, pad + ch / 2)), -1);  // past trim
    EXPECT_EQ(h.w.accessibleOffsetAt(QPoint(pad + cw / 2, pad + 2 * ch + ch / 2)), -1);  // dropped blank row

    // '\n' separator offset -> null rect.
    Harness h2;
    h2.feed("ab\r\ncd");                       // text "ab\ncd"; offset 2 is '\n'
    EXPECT_TRUE(h2.w.accessibleRectForOffset(2).isNull());
}

// INV-7 — the slot guards on QAccessible::isActive() before any work
// (bounded source scan of just the function body).
TEST(TerminalA11y, InactiveGuardSourceCheck) {
    QFile f(QStringLiteral(SRC_TERMINALWIDGET_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString src = QString::fromUtf8(f.readAll());
    const int fn = src.indexOf(QStringLiteral("void TerminalWidget::notifyAccessibilityChanged()"));
    ASSERT_GE(fn, 0);
    const int bodyEnd = src.indexOf(QStringLiteral("\n}"), fn);
    ASSERT_GT(bodyEnd, fn);
    const QString body = src.mid(fn, bodyEnd - fn);

    const int guard = body.indexOf(QStringLiteral("QAccessible::isActive()"));
    const int caret = body.indexOf(QStringLiteral("accessibleCaretOffset("));
    const int evtCall = body.indexOf(QStringLiteral("QAccessible::updateAccessibility("));
    ASSERT_GE(guard, 0);
    ASSERT_GE(caret, 0);
    ASSERT_GE(evtCall, 0);
    EXPECT_LT(guard, caret) << "isActive() guard must precede the text build";
    EXPECT_LT(guard, evtCall) << "isActive() guard must precede the event";
}

// INV-8 — the adapter holds no content cache (no data members at all).
TEST(TerminalA11y, NoContentCacheSourceCheck) {
    QFile f(QStringLiteral(SRC_TERMINALACCESSIBLE_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString hdr = QString::fromUtf8(f.readAll());
    EXPECT_FALSE(hdr.contains(QStringLiteral("m_")))
        << "adapter must declare no data members (no content cache)";
    EXPECT_FALSE(hdr.contains(QStringLiteral("std::vector")));
    EXPECT_FALSE(hdr.contains(QStringLiteral("QByteArray")));
}

// INV-9 — one caret event per invocation when active, carrying the offset.
//
// Activation is platform/Qt-version dependent: QAccessible::setActive(true)
// only flips QAccessible::isActive() when setActive() forwards to the platform
// accessibility object (Qt 6.8+). Older distro Qt — e.g. CI's Ubuntu 6.4.x —
// only notifies activation observers, so under the offscreen plugin (no AT
// bridge) isActive() stays false. The production guard in
// notifyAccessibilityChanged() then correctly suppresses the event ("zero cost
// when no AT"), and we cannot observe the delivered event on that platform.
// So assert whichever contract the platform actually exposes:
//   isActive() true  -> exactly one TextCaretMoved per call, carrying the offset.
//   isActive() false -> no events at all (the zero-cost-when-inactive guarantee).
TEST(TerminalA11y, OneEventPerInvocationWhenActive) {
    Harness h;
    h.feed("abc");                                  // caret at offset 3
    installTerminalAccessibilityFactory();          // so the event resolves a real interface
    QAccessible::UpdateHandler prev = QAccessible::installUpdateHandler(spyHandler);
    QAccessible::setActive(true);
    g_evtCount = 0;
    g_lastCursorPos = -1;
    for (int i = 0; i < 3; ++i)
        QMetaObject::invokeMethod(&h.w, "notifyAccessibilityChanged", Qt::DirectConnection);
    if (QAccessible::isActive()) {
        EXPECT_EQ(g_evtCount, 3);
        EXPECT_EQ(g_lastCursorPos, h.w.accessibleCaretOffset());
    } else {
        EXPECT_EQ(g_evtCount, 0) << "guard must suppress events when AT inactive";
    }
    QAccessible::installUpdateHandler(prev);
}
