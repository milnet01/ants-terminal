// ANTS-1217 — bundle main for GUI-flavoured GoogleTest bundles.
// Used by test_vt, test_chrome, test_claude, test_audit, test_dialogs,
// test_lua. Sets QT_QPA_PLATFORM=offscreen *before* QApplication construction
// (after-construction is too late). app.exec() is intentionally not called —
// Ants tests are pure invariant checks; tests that need event-loop dispatch
// call processEvents() or use QSignalSpy from inside their TEST block.
#include <gtest/gtest.h>
#include <QApplication>
#include <QTemporaryDir>

#include <cstdio>

int main(int argc, char *argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // ANTS-3856 — see tests/bundle_main_core.cpp for the whole reasoning: a
    // per-process XDG_DATA_HOME, so a test that omits its store path opens a
    // throwaway roadmap store instead of the user's live one.
    QTemporaryDir dataHome;
    if (!dataHome.isValid()) {
        std::fprintf(stderr, "cannot create the XDG_DATA_HOME sandbox: %s\n",
                     qUtf8Printable(dataHome.errorString()));
        return 1;  // fail closed — unsandboxed, a test can write real data.
    }
    qputenv("XDG_DATA_HOME", dataHome.path().toLocal8Bit());

    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
