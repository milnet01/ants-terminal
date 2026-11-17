// ANTS-6531 — legacy key encoding carries Alt, Shift and Ctrl.
// Contract: tests/features/legacy_key_modifiers/spec.md.

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

    void feed(const std::string &s) { parser.feed(s.data(), static_cast<int>(s.size())); }

    QByteArray key(int k, Qt::KeyboardModifiers mods, const QString &text = {}) {
        QKeyEvent ev(QEvent::KeyPress, k, mods, text);
        return w.encodeKey(&ev);
    }
};

}  // namespace

// INV-1
TEST(LegacyKeyModifiers, AltTextKeyPrefixesEsc) {
    Harness h;
    EXPECT_EQ(h.key(Qt::Key_B, Qt::AltModifier, QStringLiteral("b")),
              QByteArray("\x1B" "b"));
}

// INV-2
TEST(LegacyKeyModifiers, AltBackspacePrefixesEsc) {
    Harness h;
    EXPECT_EQ(h.key(Qt::Key_Backspace, Qt::AltModifier), QByteArray("\x1B\x7F"));
}

// INV-3
TEST(LegacyKeyModifiers, AltCtrlChordPrefixesEsc) {
    Harness h;
    EXPECT_EQ(h.key(Qt::Key_A, Qt::ControlModifier | Qt::AltModifier,
                    QStringLiteral("\x01")),
              QByteArray("\x1B\x01"));
}

// INV-4
TEST(LegacyKeyModifiers, ModifiedArrowCarriesParameter) {
    Harness h;
    EXPECT_EQ(h.key(Qt::Key_Up, Qt::ShiftModifier), QByteArray("\x1B[1;2A"));
    EXPECT_EQ(h.key(Qt::Key_Left, Qt::AltModifier), QByteArray("\x1B[1;3D"));
    EXPECT_EQ(h.key(Qt::Key_Left, Qt::ControlModifier | Qt::AltModifier),
              QByteArray("\x1B[1;7D"));
    h.feed("\x1B[?1h");  // DECCKM: application cursor keys
    ASSERT_TRUE(h.w.grid()->applicationCursorKeys());
    EXPECT_EQ(h.key(Qt::Key_Up, Qt::ShiftModifier), QByteArray("\x1B[1;2A"));
}

// INV-5
TEST(LegacyKeyModifiers, ModifiedFunctionKeyCarriesParameter) {
    Harness h;
    EXPECT_EQ(h.key(Qt::Key_F1, Qt::ShiftModifier), QByteArray("\x1B[1;2P"));
    EXPECT_EQ(h.key(Qt::Key_F5, Qt::ShiftModifier), QByteArray("\x1B[15;2~"));
    EXPECT_EQ(h.key(Qt::Key_Delete, Qt::ControlModifier), QByteArray("\x1B[3;5~"));
}

// INV-6
TEST(LegacyKeyModifiers, UnmodifiedKeysUnchanged) {
    Harness h;
    EXPECT_EQ(h.key(Qt::Key_B, Qt::NoModifier, QStringLiteral("b")), QByteArray("b"));
    EXPECT_EQ(h.key(Qt::Key_Up, Qt::NoModifier), QByteArray("\x1B[A"));
    EXPECT_EQ(h.key(Qt::Key_F1, Qt::NoModifier), QByteArray("\x1BOP"));
    EXPECT_EQ(h.key(Qt::Key_A, Qt::ShiftModifier, QStringLiteral("A")), QByteArray("A"));
}
