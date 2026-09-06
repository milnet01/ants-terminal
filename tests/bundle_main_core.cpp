// ANTS-1217 — bundle main for Qt::Core-only GoogleTest bundles.
// Used by test_core (grep-style audit-rule tests + helper-CLI tests). No GUI
// symbols are pulled in, so QApplication / Qt6::Widgets is not required.
#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QTemporaryDir>

#include <cstdio>

int main(int argc, char *argv[]) {
    // ANTS-3856 — sandbox XDG_DATA_HOME before any test runs, so nothing in
    // this process can reach the user's real data directory.
    //
    // The case that forced it: RoadmapStore::defaultPath() resolves under
    // GenericDataLocation and the constructor's dbPath DEFAULTS to it, so
    // `RoadmapStore store;` in a test opens the developer's live roadmap
    // database and passes. A fixture project rooted at /tmp/test_core-ZnzBrv
    // was found registered in it on 2026-08-06. Now defaultPath() resolves
    // here instead, for every reader — including the ones that only stat it
    // (RoadmapSource::storeFor()) and never construct a store, which a
    // store-side redirect would have desynchronised.
    //
    // The env var and not a store-specific knob: this is the mechanism the
    // suite already uses per-test (ants_test::XdgGuard, ANTS-2062) and it
    // covers every writer under GenericDataLocation, not only the store.
    // A test that needs its own sandbox still overrides this one.
    //
    // Per process, not a fixed path: ctest runs at -j4 and two processes
    // sharing one data dir would see each other's files. The dir outlives
    // every test and is removed when this scope ends.
    QTemporaryDir dataHome;
    if (!dataHome.isValid()) {
        std::fprintf(stderr, "cannot create the XDG_DATA_HOME sandbox: %s\n",
                     qUtf8Printable(dataHome.errorString()));
        return 1;  // fail closed — unsandboxed, a test can write real data.
    }
    qputenv("XDG_DATA_HOME", dataHome.path().toLocal8Bit());

    // ANTS-4898 — the same guard for XDG_CONFIG_HOME, which ANTS-3856 did not
    // cover. Config::load() reads ~/.config/ants-terminal/config.json on
    // construction, so every verb consulting a setting consulted the
    // developer's own.
    //
    // The case that forced it: `claude.mcp_feedback_root` (ANTS-4471) was set
    // to the shared feedback corpus — which is what that key is for. Twelve
    // McpFeedbackQuery / McpFeedbackLog tests, each asserting a path derived
    // under its own QTemporaryDir, went red because the live key redirected
    // the derivation. The red was the harmless half: the writes landed, in
    // the user's real corpus, on 2026-09-06.
    //
    // A test whose result depends on a setting outside the repo is not a
    // test. A test that wants its own config still overrides this one, as
    // ants_test::XdgGuard does.
    QTemporaryDir configHome;
    if (!configHome.isValid()) {
        std::fprintf(stderr, "cannot create the XDG_CONFIG_HOME sandbox: %s\n",
                     qUtf8Printable(configHome.errorString()));
        return 1;  // fail closed — unsandboxed, a test can write real files.
    }
    qputenv("XDG_CONFIG_HOME", configHome.path().toLocal8Bit());

    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
