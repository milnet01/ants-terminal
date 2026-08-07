// Feature-conformance test for ANTS-3856 — the test harness cannot reach the
// user's data directory. Contract: tests/features/roadmap_store_sandbox/spec.md
//
// Behavioural, except INV-5: the arming lives in main(), so the only way to
// assert the GUI bundles are armed too is to read their source. This test
// compiles into test_core alone.

#include <gtest/gtest.h>

#include "roadmapstore.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace {

// Where the user's data really lives, derived from HOME rather than from
// QStandardPaths — which is the thing under test and would agree with itself.
QString realDataHome() {
    const QString home = qEnvironmentVariable("HOME");
    return home.isEmpty() ? QString() : home + QStringLiteral("/.local/share");
}

}  // namespace

// INV-1 — armed for the whole run, from main(), before any test body, and
// pointing somewhere that is not the user's data directory.
TEST(RoadmapStoreSandbox, Inv1HarnessIsArmed) {
    const QString dataHome = qEnvironmentVariable("XDG_DATA_HOME");
    ASSERT_FALSE(dataHome.isEmpty())
        << "the bundle main did not sandbox XDG_DATA_HOME — every other "
           "invariant here is vacuous without it";
    EXPECT_TRUE(QDir(dataHome).exists()) << dataHome.toStdString();

    const QString real = realDataHome();
    if (!real.isEmpty()) {
        EXPECT_NE(QDir::cleanPath(dataHome), QDir::cleanPath(real))
            << "the sandbox IS the user's data directory — that is no sandbox";
    }
}

// INV-2 — defaultPath() resolves inside the sandbox. Asserted on defaultPath()
// and not on a constructed store because the readers that matter most never
// construct one: RoadmapSource::storeFor() STATS this path and returns nullptr
// when it is absent. A redirect applied store-side rather than here leaves the
// stat looking at one file and the open at another.
TEST(RoadmapStoreSandbox, Inv2DefaultPathResolvesIntoTheSandbox) {
    const QString dataHome = qEnvironmentVariable("XDG_DATA_HOME");
    ASSERT_FALSE(dataHome.isEmpty());
    EXPECT_EQ(RoadmapStore::defaultPath(),
              dataHome + QStringLiteral("/ants-terminal/roadmap.sqlite"));

    const QString real = realDataHome();
    if (!real.isEmpty()) {
        EXPECT_FALSE(RoadmapStore::defaultPath().startsWith(QDir::cleanPath(real)))
            << "the store still resolves into the user's data directory";
    }
}

// INV-3 — the exact shape that leaked: no path at all, so the constructor
// resolves defaultPath(). It must open, and it must open in the sandbox.
TEST(RoadmapStoreSandbox, Inv3DefaultConstructedStoreIsSandboxed) {
    const QString dataHome = qEnvironmentVariable("XDG_DATA_HOME");
    ASSERT_FALSE(dataHome.isEmpty());

    RoadmapStore store;
    EXPECT_TRUE(store.path().startsWith(dataHome))
        << "a default-constructed store resolved outside the sandbox: "
        << store.path().toStdString();

    // ASSERT, and against the HOME-derived path rather than $XDG_DATA_HOME:
    // this test WRITES below, and the check above is vacuous when the arming
    // is absent but the variable is inherited from the developer's shell —
    // which is the common case (XDG_DATA_HOME=$HOME/.local/share is exported
    // on this machine). Measured 2026-08-07: with EXPECT and no HOME-derived
    // comparison, disarming main() made this test register a fixture project
    // in the live store — the very leak the item exists to close.
    const QString real = realDataHome();
    if (!real.isEmpty()) {
        ASSERT_FALSE(store.path().startsWith(QDir::cleanPath(real)))
            << "refusing to write: the store resolved into the user's real "
               "data directory at " << store.path().toStdString();
    }

    QString err;
    ASSERT_TRUE(store.open(&err)) << err.toStdString();
    EXPECT_TRUE(QFile::exists(store.path()));

    // And it is a real store: the leak was a registerProject() away.
    const QString root = dataHome + QStringLiteral("/proj");
    ASSERT_TRUE(QDir().mkpath(root));
    EXPECT_TRUE(store.registerProject(root, QStringLiteral("Demo"),
                                      QStringLiteral("demo"), &err)
                    .has_value())
        << err.toStdString();
}

// INV-4 — a store given a path of its own is untouched by the sandbox. Every
// other roadmap test in the suite is this case.
TEST(RoadmapStoreSandbox, Inv4ExplicitOwnPathIsUntouched) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString wanted = dir.filePath(QStringLiteral("roadmap.sqlite"));
    RoadmapStore store(wanted);
    EXPECT_EQ(store.path(), wanted);

    QString err;
    ASSERT_TRUE(store.open(&err)) << err.toStdString();
    EXPECT_TRUE(QFile::exists(wanted));
}

// INV-5 — the GUI bundles are armed too, and this test never runs in one.
// Scraped rather than assumed: these two files are main() for every bundle in
// the project, and nothing else can reach across into them.
TEST(RoadmapStoreSandbox, Inv5EveryBundleMainArmsIt) {
    const QDir testsDir(QStringLiteral(ANTS_TESTS_DIR));
    const QStringList mains =
        testsDir.entryList({QStringLiteral("bundle_main_*.cpp")}, QDir::Files, QDir::Name);
    ASSERT_FALSE(mains.isEmpty()) << "no bundle mains found under " << ANTS_TESTS_DIR;

    for (const QString &name : mains) {
        QFile f(testsDir.filePath(name));
        ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text)) << name.toStdString();
        const QString src = QString::fromUtf8(f.readAll());
        EXPECT_TRUE(src.contains(QStringLiteral("qputenv(\"XDG_DATA_HOME\"")))
            << name.toStdString() << " does not sandbox XDG_DATA_HOME — every "
            << "bundle it is main() for can reach the user's real data directory";
        EXPECT_TRUE(src.contains(QStringLiteral("QTemporaryDir")))
            << name.toStdString() << " must sandbox to a PER-PROCESS directory: "
            << "ctest runs the bundles in parallel and a shared one would let "
            << "two processes see each other's files";
    }
}
