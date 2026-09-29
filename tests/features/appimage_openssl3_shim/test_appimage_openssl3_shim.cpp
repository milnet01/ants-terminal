// ANTS-5574 — the AppImage's OpenSSL 3 shim directory stays out of every
// process the terminal starts.
//
// Contract:
//   INV-1  withoutEntry removes every entry equal to the shim directory and
//          keeps every other entry, empty ones included, in its place.
//   INV-2  dropOpenSsl3Shim removes the shim directory from LD_LIBRARY_PATH,
//          unsets LD_LIBRARY_PATH when nothing is left, and unsets
//          ANTS_OPENSSL3_SHIM.
//   INV-3  with ANTS_OPENSSL3_SHIM unset, dropOpenSsl3Shim leaves
//          LD_LIBRARY_PATH as it was.
//   INV-4  main() calls dropOpenSsl3Shim before QApplication is built.
//
// Why this exists: AppRun puts the shim first on LD_LIBRARY_PATH so the
// bundled Qt 6.2 finds OpenSSL 3. Left in the environment, every shell in a
// tab would inherit it and resolve an unversioned libssl.so differently from
// the same shell outside Ants.

#include "appimageenv.h"

#include "../../_support/expect.h"

#include <gtest/gtest.h>

#include <QFile>

ANTS_TEST_SCOPE();

namespace {

// Saves and restores the two variables a test touches.
struct EnvGuard {
    QByteArray ld, shim;
    bool hadLd, hadShim;
    EnvGuard()
        : ld(qgetenv("LD_LIBRARY_PATH")), shim(qgetenv("ANTS_OPENSSL3_SHIM")),
          hadLd(qEnvironmentVariableIsSet("LD_LIBRARY_PATH")),
          hadShim(qEnvironmentVariableIsSet("ANTS_OPENSSL3_SHIM")) {}
    ~EnvGuard() {
        if (hadLd) qputenv("LD_LIBRARY_PATH", ld); else qunsetenv("LD_LIBRARY_PATH");
        if (hadShim) qputenv("ANTS_OPENSSL3_SHIM", shim); else qunsetenv("ANTS_OPENSSL3_SHIM");
    }
};

}  // namespace

TEST(AppImageOpenSsl3Shim, Inv1WithoutEntry) {
    using AppImageEnv::withoutEntry;
    EXPECT_EQ(withoutEntry("/shim", "/shim"), QByteArray());
    EXPECT_EQ(withoutEntry("/shim:/usr/lib", "/shim"), QByteArray("/usr/lib"));
    EXPECT_EQ(withoutEntry("/a:/shim:/b", "/shim"), QByteArray("/a:/b"));
    EXPECT_EQ(withoutEntry("/a:/shim", "/shim"), QByteArray("/a"));
    EXPECT_EQ(withoutEntry("/shim:/a:/shim", "/shim"), QByteArray("/a"));
    // A prefix of the entry is a different directory.
    EXPECT_EQ(withoutEntry("/shim2:/shim/x", "/shim"), QByteArray("/shim2:/shim/x"));
    // An empty entry (the current directory) is the user's, and stays.
    EXPECT_EQ(withoutEntry("/shim::/a", "/shim"), QByteArray(":/a"));
    EXPECT_EQ(withoutEntry("/a:/b", "/shim"), QByteArray("/a:/b"));
}

TEST(AppImageOpenSsl3Shim, Inv2DropRemovesShim) {
    EnvGuard g;
    qputenv("ANTS_OPENSSL3_SHIM", "/run/user/1000/ants-terminal-openssl3");
    qputenv("LD_LIBRARY_PATH", "/run/user/1000/ants-terminal-openssl3:/opt/lib");
    AppImageEnv::dropOpenSsl3Shim();
    EXPECT_EQ(qgetenv("LD_LIBRARY_PATH"), QByteArray("/opt/lib"));
    EXPECT_FALSE(qEnvironmentVariableIsSet("ANTS_OPENSSL3_SHIM"));

    qputenv("ANTS_OPENSSL3_SHIM", "/run/user/1000/ants-terminal-openssl3");
    qputenv("LD_LIBRARY_PATH", "/run/user/1000/ants-terminal-openssl3");
    AppImageEnv::dropOpenSsl3Shim();
    EXPECT_FALSE(qEnvironmentVariableIsSet("LD_LIBRARY_PATH"));
    EXPECT_FALSE(qEnvironmentVariableIsSet("ANTS_OPENSSL3_SHIM"));
}

TEST(AppImageOpenSsl3Shim, Inv3NoShimNoChange) {
    EnvGuard g;
    qunsetenv("ANTS_OPENSSL3_SHIM");
    qputenv("LD_LIBRARY_PATH", "/run/user/1000/ants-terminal-openssl3:/opt/lib");
    AppImageEnv::dropOpenSsl3Shim();
    EXPECT_EQ(qgetenv("LD_LIBRARY_PATH"),
              QByteArray("/run/user/1000/ants-terminal-openssl3:/opt/lib"));
}

TEST(AppImageOpenSsl3Shim, Inv4MainDropsShimFirst) {
    QFile f(QStringLiteral(SRC_MAIN_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly)) << "read src/main.cpp";
    const QByteArray src = f.readAll();
    const qsizetype call = src.indexOf("AppImageEnv::dropOpenSsl3Shim()");
    const qsizetype app = src.indexOf("QApplication app(");
    ASSERT_GE(call, 0) << "main() never calls dropOpenSsl3Shim";
    ASSERT_GE(app, 0) << "QApplication app( not found in main.cpp";
    EXPECT_LT(call, app) << "dropOpenSsl3Shim runs after QApplication";
}
