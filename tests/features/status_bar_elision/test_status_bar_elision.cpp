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

#include <QApplication>
#include <QStatusBar>
#include <QMainWindow>
#include <QFontMetrics>

#include <cstdio>

namespace {

int failures = 0;

#define CHECK(cond, msg) do {                                                \
    if (!(cond)) {                                                           \
        std::fprintf(stderr, "FAIL %s:%d — %s\n", __FILE__, __LINE__, msg);  \
        ++failures;                                                          \
    }                                                                        \
} while (0)

// Invariant 1 + 3: short text in a capped label must render in full and
// its minimumSizeHint must allow the full-text render.
void testShortTextUnderCap() {
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
void testLongTextOverCap() {
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
void testStatusBarLayoutDoesNotSqueeze() {
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

}  // namespace

int main(int argc, char **argv) {
    // QApplication (not QCoreApplication) — QLabel/QFontMetrics needs GUI.
    QApplication app(argc, argv);

    testShortTextUnderCap();
    testLongTextOverCap();
    testStatusBarLayoutDoesNotSqueeze();

    if (failures > 0) {
        std::fprintf(stderr, "\n%d assertion(s) failed.\n", failures);
        return 1;
    }
    std::fprintf(stderr, "All ElidedLabel invariants hold.\n");
    return 0;
}
