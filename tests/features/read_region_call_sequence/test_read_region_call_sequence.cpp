// Feature-conformance test for ANTS-2157 — read_region `call_sequence`:
// the integration-brief view (ordered pipeline stages + insertion points +
// accessors) implemented reuse-first as an option on read_region's
// symbol-body mode rather than a new verb. Drives the pure
// ReadRegion::extract; wiring is source-grepped. See spec.md + ROADMAP
// ANTS-2157.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"
#include "readregion.h"

#include <gtest/gtest.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <string>

#ifndef ANTS_SOURCE_DIR
#error "ANTS_SOURCE_DIR compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {
bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}
QStringList callees(const QJsonObject &env) {
    QStringList out;
    for (const auto &v : env.value(QStringLiteral("call_sequence")).toArray())
        out << v.toObject().value(QStringLiteral("callee")).toString();
    return out;
}
QStringList accessorsOf(const QJsonObject &env) {
    QStringList out;
    for (const auto &v : env.value(QStringLiteral("accessors")).toArray())
        out << v.toString();
    return out;
}
}  // namespace

// INV-1 — call_sequence lists the body's calls in source order (the
// pipeline stages), skips the signature line, stops at the symbol's end,
// and accessors captures the m_ members + getters referenced.
TEST(ReadRegionCallSequence, OrderedStagesAndAccessors) {
    QTemporaryDir dir;
    const QString root = QFileInfo(dir.path()).canonicalFilePath();
    QDir().mkpath(root + "/src");
    const QString path = root + "/src/pipeline.cpp";
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(
        "void renderFrame() {\n"
        "    shadowPass();\n"
        "    geometryPass();\n"
        "    auto sm = getCascadedShadowMap();\n"
        "    compositePass(m_directionalLight, sm);\n"
        "    presentPass();\n"
        "}\n"
        "int helperBelow() { return 0; }\n");
    f.close();

    ReadRegion::Options opts;
    opts.symbol = QStringLiteral("renderFrame");
    opts.callSequence = true;
    const QJsonObject env = ReadRegion::extract(path, opts);
    ASSERT_TRUE(env.value("ok").toBool());

    const QStringList seq = callees(env);
    // Ordered stages; signature 'renderFrame' is NOT a stage.
    ASSERT_GE(seq.size(), 5);
    EXPECT_EQ(seq.first(), QStringLiteral("shadowPass"));
    EXPECT_EQ(seq.indexOf(QStringLiteral("geometryPass")), 1);
    EXPECT_LT(seq.indexOf(QStringLiteral("compositePass")),
              seq.indexOf(QStringLiteral("presentPass")));
    EXPECT_FALSE(seq.contains(QStringLiteral("renderFrame")));   // signature skipped
    EXPECT_FALSE(seq.contains(QStringLiteral("helperBelow")));   // outside the symbol range

    const QStringList acc = accessorsOf(env);
    EXPECT_TRUE(acc.contains(QStringLiteral("m_directionalLight")));    // member
    EXPECT_TRUE(acc.contains(QStringLiteral("getCascadedShadowMap"))); // getter
}

// INV-2 — call_sequence is opt-in: absent the flag, the response carries no
// call_sequence / accessors (back-compat for plain read_region callers).
TEST(ReadRegionCallSequence, OptInOnly) {
    QTemporaryDir dir;
    const QString root = QFileInfo(dir.path()).canonicalFilePath();
    QDir().mkpath(root + "/src");
    const QString path = root + "/src/p.cpp";
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("void go() {\n    stepOne();\n}\n");
    f.close();

    ReadRegion::Options opts;
    opts.symbol = QStringLiteral("go");   // callSequence defaults false
    const QJsonObject env = ReadRegion::extract(path, opts);
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_FALSE(env.contains("call_sequence"));
    EXPECT_FALSE(env.contains("accessors"));
}

// INV-3 — wiring: cmdReadRegion threads call_sequence into Options and the
// schema advertises it.
TEST(ReadRegionCallSequence, Wiring) {
    const std::string rc = ants_test::slurpFile(std::string(ANTS_SOURCE_DIR) + "/src/remotecontrol.cpp");
    const std::string ci = ants_test::slurpFile(std::string(ANTS_SOURCE_DIR) + "/src/claudeintegration.cpp");
    EXPECT_TRUE(has(rc, "call_sequence"));
    EXPECT_TRUE(has(rc, "opts.callSequence"));
    EXPECT_TRUE(has(ci, "call_sequence"));
}
