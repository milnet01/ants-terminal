// Feature-conformance test for ANTS-2156 — similar_code `include_bodies`:
// return the FULL enclosing definition for the top matches so a session
// copies an in-repo idiom in one call instead of N follow-up Reads.
// The verb glue (cmdSimilarCode) needs RemoteControl, so the behavioural
// half drives the two pure libs it composes — SimilarCode::findSimilar +
// ReadRegion::extract — and the wiring half source-greps the handler +
// schema. See spec.md + ROADMAP ANTS-2156.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"
#include "similarcode.h"
#include "readregion.h"

#include <gtest/gtest.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
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
std::string srcPath(const char *rel) {
    return std::string(ANTS_SOURCE_DIR) + "/" + rel;
}
// Mirror of remotecontrol.cpp's scSymbolFromSignature (func path) so the
// test exercises the same parse → extract chain the verb performs.
QString symFromFuncSig(const QString &sig) {
    QString s = sig.trimmed();
    const int lp = s.indexOf(QLatin1Char('('));
    if (lp > 0) s = s.left(lp).trimmed();
    const int sp = s.lastIndexOf(QLatin1Char(' '));
    if (sp >= 0) s = s.mid(sp + 1);
    while (!s.isEmpty() && (s.front() == QLatin1Char('*') || s.front() == QLatin1Char('&')))
        s.remove(0, 1);
    return s;
}
}  // namespace

// INV-1 — findSimilar + ReadRegion compose: a shape query locates a func,
// and its signature resolves to the full body via the symbol extractor.
TEST(SimilarCodeBodies, FindThenExtractFullBody) {
    QTemporaryDir dir;
    const QString root = QFileInfo(dir.path()).canonicalFilePath();
    QDir().mkpath(root + "/src");
    QFile f(root + "/src/widget.cpp");
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(
        "void renderFrame(int w, int h) {\n"
        "    int total = w * h;\n"
        "    (void)total;\n"
        "}\n"
        "int unrelatedThing(int n) { return n; }\n");
    f.close();

    const SimilarCode::Result res =
        SimilarCode::findSimilar(root, QStringLiteral("void renderFrame(int w, int h)"), {});
    ASSERT_TRUE(res.ok);
    ASSERT_FALSE(res.matches.isEmpty());
    const SimilarCode::Match &top = res.matches.first();
    EXPECT_EQ(top.signature.contains(QStringLiteral("renderFrame")), true);

    ReadRegion::Options ro;
    ro.symbol = symFromFuncSig(top.signature);
    EXPECT_EQ(ro.symbol, QStringLiteral("renderFrame"));
    const QJsonObject body =
        ReadRegion::extract(root + "/" + top.file, ro);
    ASSERT_TRUE(body.value("ok").toBool());
    // The extracted body spans the whole definition (signature line +
    // interior), i.e. more than one line.
    EXPECT_GE(body.value("lines").toArray().size(), 3);
    bool sawInterior = false;
    for (const auto &lv : body.value("lines").toArray())
        if (lv.toString().contains(QStringLiteral("w * h"))) sawInterior = true;
    EXPECT_TRUE(sawInterior) << "full body should include the function interior";
}

// INV-2 — verb + schema wiring: cmdSimilarCode honours include_bodies and
// reuses ReadRegion; the schema advertises the option.
TEST(SimilarCodeBodies, VerbAndSchemaWiring) {
    const std::string rc = ants_test::slurpFile(srcPath("src/remotecontrol.cpp"));
    const std::string ci = ants_test::slurpFile(srcPath("src/claudeintegration.cpp"));
    EXPECT_TRUE(has(rc, "include_bodies"));
    EXPECT_TRUE(has(rc, "ReadRegion::extract"));
    EXPECT_TRUE(has(rc, "scSymbolFromSignature"));
    EXPECT_TRUE(has(rc, "body_unavailable"));
    EXPECT_TRUE(has(ci, "include_bodies"));   // advertised in the similar_code schema
}
