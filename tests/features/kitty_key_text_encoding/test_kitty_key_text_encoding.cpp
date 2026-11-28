// ANTS-6654 — Kitty keyboard protocol: text keys with modifiers.
// Contract: tests/features/kitty_key_text_encoding/spec.md.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QKeyEvent>
#include <QString>

#include <string>

#include "terminalgrid.h"
#include "terminalwidget.h"
#include "vtparser.h"

namespace {

struct Harness {
    TerminalWidget w;
    VtParser parser{[this](const VtAction &a) { w.grid()->processAction(a); }};

    explicit Harness(int flags) {
        const std::string push = "\x1B[>" + std::to_string(flags) + "u";
        parser.feed(push.data(), static_cast<int>(push.size()));
    }

    QByteArray key(int k, Qt::KeyboardModifiers mods, const QString &text = {}) {
        QKeyEvent ev(QEvent::KeyPress, k, mods, text);
        return w.encodeKey(&ev);
    }
};

}  // namespace

// INV-1
TEST(KittyKeyTextEncoding, CtrlLetterUnderDisambiguate) {
    Harness h(1);
    ASSERT_EQ(h.w.grid()->kittyKeyFlags(), 1);
    EXPECT_EQ(h.key(Qt::Key_A, Qt::ControlModifier, QStringLiteral("\x01")),
              QByteArray("\x1B[97;5u"));
    EXPECT_EQ(h.key(Qt::Key_A, Qt::ControlModifier | Qt::AltModifier,
                    QStringLiteral("\x01")),
              QByteArray("\x1B[97;7u"));
}

// INV-2
TEST(KittyKeyTextEncoding, CtrlLetterUnderAllKeys) {
    Harness h(8);
    ASSERT_EQ(h.w.grid()->kittyKeyFlags(), 8);
    EXPECT_EQ(h.key(Qt::Key_A, Qt::ControlModifier, QStringLiteral("\x01")),
              QByteArray("\x1B[97;5u"));
    EXPECT_EQ(h.key(Qt::Key_A, Qt::NoModifier, QStringLiteral("a")),
              QByteArray("\x1B[97u"));
}

// INV-3
TEST(KittyKeyTextEncoding, ShiftAloneIsText) {
    Harness h(1);
    EXPECT_EQ(h.key(Qt::Key_A, Qt::ShiftModifier, QStringLiteral("A")), QByteArray("A"));
    EXPECT_EQ(h.key(Qt::Key_A, Qt::AltModifier, QStringLiteral("a")),
              QByteArray("\x1B[97;3u"));
}
