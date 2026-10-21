// ANTS-5503 — feature-conformance test for TestRunners::detect. See spec.md
// and docs/specs/ANTS-5503-multi-runner-tests.md (INV-1).

#include <gtest/gtest.h>

#include "testrunners.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <initializer_list>
#include <utility>

namespace TR = TestRunners;
using TR::Runner;

namespace {

using Files = std::initializer_list<std::pair<const char *, const char *>>;

// One scratch root holding `files`; returns detect() over it.
TR::Detection detectIn(Files files) {
    QTemporaryDir td;
    for (const auto &[name, body] : files) {
        const QString path = td.path() + QLatin1Char('/') + QString::fromLatin1(name);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) f.write(body);
    }
    return TR::detect(td.path());
}

void expectDetected(Files files, Runner r, const char *evidence) {
    const TR::Detection d = detectIn(files);
    ASSERT_TRUE(d.runner.has_value()) << evidence;
    EXPECT_EQ(*d.runner, r) << evidence;
    EXPECT_EQ(d.evidence, QString::fromLatin1(evidence));
}

}  // namespace

// INV-1 — one scratch root per row of § 2.2's table.
TEST(TestRunnersDetect, Inv1EachMarker) {
    expectDetected({{"CMakeLists.txt", "project(x)\n"}}, Runner::Ctest, "CMakeLists.txt");
    expectDetected({{"Cargo.toml", "[package]\nname = \"x\"\n"}}, Runner::Cargo, "Cargo.toml");
    expectDetected({{"go.mod", "module x\n"}}, Runner::Go, "go.mod");
    expectDetected({{"package.json", R"({"devDependencies":{"vitest":"^5"}})"}},
                   Runner::Vitest, "package.json");
    expectDetected({{"package.json", R"({"dependencies":{"jest":"^30"}})"}},
                   Runner::Jest, "package.json");
    expectDetected({{"package.json", R"({"name":"x","jest":{"testEnvironment":"node"}})"}},
                   Runner::Jest, "package.json");
    expectDetected({{"pytest.ini", "[pytest]\n"}}, Runner::Pytest, "pytest.ini");
    expectDetected({{"conftest.py", ""}}, Runner::Pytest, "conftest.py");
    expectDetected({{"pyproject.toml", "[tool.pytest.ini_options]\n"}},
                   Runner::Pytest, "pyproject.toml");
    expectDetected({{"setup.cfg", "[tool:pytest]\n"}}, Runner::Pytest, "setup.cfg");
}

// INV-1 — order: ctest before jest, vitest before jest.
TEST(TestRunnersDetect, Inv1OrderWins) {
    expectDetected({{"CMakeLists.txt", "project(x)\n"},
                    {"package.json", R"({"devDependencies":{"jest":"^30"}})"}},
                   Runner::Ctest, "CMakeLists.txt");
    expectDetected({{"package.json",
                     R"({"devDependencies":{"jest":"^30","vitest":"^5"}})"}},
                   Runner::Vitest, "package.json");
}

// INV-1 — a package.json naming neither, a pyproject.toml without pytest, and
// an empty root detect nothing.
TEST(TestRunnersDetect, Inv1NoMarkerNoRunner) {
    EXPECT_FALSE(detectIn({{"package.json", R"({"devDependencies":{"mocha":"^10"}})"}})
                     .runner.has_value());
    EXPECT_FALSE(detectIn({{"pyproject.toml", "[tool.black]\n"}}).runner.has_value());
    const TR::Detection none = detectIn({});
    EXPECT_FALSE(none.runner.has_value());
    EXPECT_TRUE(none.evidence.isEmpty());
}

TEST(TestRunnersDetect, NamesRoundTrip) {
    for (Runner r : {Runner::Ctest, Runner::Pytest, Runner::Jest, Runner::Vitest,
                     Runner::Cargo, Runner::Go})
        EXPECT_EQ(TR::fromName(TR::name(r)), r);
    EXPECT_FALSE(TR::fromName(QStringLiteral("mocha")).has_value());
    EXPECT_FALSE(TR::fromName(QStringLiteral("Pytest")).has_value());
}
