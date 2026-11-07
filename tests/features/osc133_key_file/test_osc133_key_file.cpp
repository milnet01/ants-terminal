// ANTS-6441 — the OSC 133 key comes from a key file before the environment.
// Contract: docs/specs/ANTS-6441-osc133-key-file.md (INV-1, INV-2, INV-3).

#include "terminalgrid.h"
#include "vtparser.h"

#include "../../_support/xdg_guard.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QMessageAuthenticationCode>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <memory>

namespace {

QByteArray fileKey() { return QByteArray(64, 'a'); }
QByteArray envKey()  { return QByteArray(64, 'b'); }

// An `A` marker for prompt `p1`, signed with `key`.
QByteArray signedA(const QByteArray &key) {
    QMessageAuthenticationCode mac(QCryptographicHash::Sha256, key);
    mac.addData(QByteArrayLiteral("A|p1"));
    return "\033]133;A;aid=p1;ahmac=" + mac.result().toHex() + "\007";
}

// Whether `grid` applies a marker signed with `key` (a forgery bumps the count).
bool accepts(TerminalGrid &grid, const QByteArray &key) {
    VtParser parser([&grid](const VtAction &a) { grid.processAction(a); });
    const int before = grid.osc133ForgeryCount();
    const QByteArray wire = signedA(key);
    parser.feed(wire.constData(), static_cast<int>(wire.size()));
    return grid.osc133ForgeryCount() == before;
}

// A private XDG_CONFIG_HOME, and no key in the environment, for each test.
struct Sandbox {
    QTemporaryDir dir;
    ants_test::XdgGuard guard;
    Sandbox() {
        guard.setTestMode(false);
        guard.setEnv("XDG_CONFIG_HOME", dir.path().toUtf8());
        guard.unsetEnv("ANTS_OSC133_KEY");
    }
    void writeKeyFile(const QByteArray &body) {
        QDir().mkpath(dir.path() + "/ants-terminal");
        QFile f(dir.path() + "/ants-terminal/osc133.key");
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(body);
    }
};

std::unique_ptr<TerminalGrid> newGrid() { return std::make_unique<TerminalGrid>(4, 80); }

}  // namespace

// INV-1
TEST(Osc133KeyFile, Inv1FileWinsOverEnv) {
    Sandbox sb;
    ASSERT_TRUE(sb.dir.isValid());
    sb.writeKeyFile(fileKey() + "\n");
    sb.guard.setEnv("ANTS_OSC133_KEY", envKey());
    auto grid = newGrid();
    EXPECT_TRUE(grid->osc133HmacEnforced());
    EXPECT_TRUE(accepts(*grid, fileKey())) << "a marker signed with the file's key";
    EXPECT_FALSE(accepts(*grid, envKey())) << "a marker signed with the env key";
}

// INV-2
TEST(Osc133KeyFile, Inv2EnvFallback) {
    Sandbox sb;
    ASSERT_TRUE(sb.dir.isValid());
    EXPECT_FALSE(newGrid()->osc133HmacEnforced()) << "no file, no env: verifier off";

    sb.guard.setEnv("ANTS_OSC133_KEY", envKey());
    auto noFile = newGrid();
    EXPECT_TRUE(accepts(*noFile, envKey())) << "no key file: the env key";

    sb.writeKeyFile("  \n");
    auto emptyFile = newGrid();
    EXPECT_TRUE(accepts(*emptyFile, envKey())) << "an empty key file: the env key";
}

// INV-3
TEST(Osc133KeyFile, Inv3NewGridReadsFile) {
    Sandbox sb;
    ASSERT_TRUE(sb.dir.isValid());
    sb.writeKeyFile(fileKey());
    auto before = newGrid();
    sb.writeKeyFile(envKey());
    auto after = newGrid();
    EXPECT_TRUE(accepts(*before, fileKey())) << "a grid built earlier keeps its key";
    EXPECT_TRUE(accepts(*after, envKey())) << "a grid built after the change reads it";
    // The verifier is on in both, so each refuses the other's key.
    EXPECT_FALSE(accepts(*before, envKey()));
    EXPECT_FALSE(accepts(*after, fileKey()));
}
