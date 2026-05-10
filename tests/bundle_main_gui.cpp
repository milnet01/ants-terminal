// ANTS-1217 — bundle main for GUI-flavoured GoogleTest bundles.
// Used by test_vt, test_chrome, test_claude, test_audit, test_dialogs,
// test_lua. Sets QT_QPA_PLATFORM=offscreen *before* QApplication construction
// (after-construction is too late). app.exec() is intentionally not called —
// Ants tests are pure invariant checks; tests that need event-loop dispatch
// call processEvents() or use QSignalSpy from inside their TEST block.
#include <gtest/gtest.h>
#include <QApplication>

int main(int argc, char *argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
