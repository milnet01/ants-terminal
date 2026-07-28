// Feature-conformance test for ANTS-2221 — read_region markdown `section:`
// selector. MD-1..MD-7 drive the pure ReadRegion::extract; MD-8 source-greps
// the handler + schema wiring. See spec.md + ROADMAP ANTS-2221 (DOOM S3).

#include "../../_support/expect.h"
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

#include "../../_support/srcgrep.h"

#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif
#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

QString joinLines(const QJsonObject &env) {
    QString out;
    for (const auto &v : env.value(QStringLiteral("lines")).toArray())
        out += v.toString() + QLatin1Char('\n');
    return out;
}

// One spec-shaped fixture. Line numbers (1-based):
//  1 # Spec Title
//  2 (blank)
//  3 Intro prose.
//  4 (blank)
//  5 ## 4.1 Overview
//  6 Overview body.
//  7 (blank)
//  8 ## 4.2 Emission model
//  9 The emission model body.
// 10 (blank)
// 11 ### 4.2.1 Details
// 12 Detail text.
// 13 (blank)
// 14 ## 5 Examples
// 15 ```bash
// 16 # not a heading — inside a fence
// 17 echo hi
// 18 ```
// 19 Trailing example prose.
// 20 (blank)
// 21 ## 6 Cleanup
// 22 Cleanup body, last section.
const char *kDoc =
    "# Spec Title\n"                  // 1
    "\n"                              // 2
    "Intro prose.\n"                  // 3
    "\n"                              // 4
    "## 4.1 Overview\n"              // 5
    "Overview body.\n"               // 6
    "\n"                              // 7
    "## 4.2 Emission model\n"        // 8
    "The emission model body.\n"     // 9
    "\n"                              // 10
    "### 4.2.1 Details\n"            // 11
    "Detail text.\n"                 // 12
    "\n"                              // 13
    "## 5 Examples\n"                // 14
    "```bash\n"                       // 15
    "# not a heading — inside a fence\n"  // 16
    "echo hi\n"                       // 17
    "```\n"                           // 18
    "Trailing example prose.\n"      // 19
    "\n"                              // 20
    "## 6 Cleanup\n"                 // 21
    "Cleanup body, last section.\n"; // 22

QJsonObject extractSection(const QString &path, const char *section) {
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(kDoc);
    f.close();
    ReadRegion::Options opts;
    opts.section = QString::fromLatin1(section);
    return ReadRegion::extract(path, opts);
}

QString writeDoc(QTemporaryDir &dir, const char *name) {
    const QString root = QFileInfo(dir.path()).canonicalFilePath();
    QDir().mkpath(root + "/docs");
    return root + "/docs/" + QLatin1String(name);
}

// ANTS-2234 — write an arbitrary doc body then resolve `section` against it.
QJsonObject extractFrom(const QString &path, const char *doc,
                        const char *section) {
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(doc);
    f.close();
    ReadRegion::Options opts;
    opts.section = QString::fromLatin1(section);
    return ReadRegion::extract(path, opts);
}

}  // namespace

// MD-1 — body by slug: heading line through the line before the next
// same-or-higher heading. Echoes section + section_slug.
TEST(ReadRegionMdSection, BodyBySlug) {
    QTemporaryDir dir;
    const QJsonObject env =
        extractSection(writeDoc(dir, "a.md"), "4-2-emission-model");
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(env.value("start_line").toInt(), 8);
    EXPECT_EQ(env.value("end_line").toInt(), 13);  // before "## 5 Examples"
    EXPECT_EQ(env.value("section_slug").toString().toStdString(),
              "4-2-emission-model");
    const QString body = joinLines(env);
    EXPECT_TRUE(body.contains(QStringLiteral("The emission model body.")));
    EXPECT_TRUE(body.contains(QStringLiteral("### 4.2.1 Details")));  // MD-3
    EXPECT_FALSE(body.contains(QStringLiteral("## 5 Examples")));
}

// MD-2 — body by heading text resolves the same range (idempotent slug).
TEST(ReadRegionMdSection, BodyByHeadingText) {
    QTemporaryDir dir;
    const QJsonObject env =
        extractSection(writeDoc(dir, "b.md"), "4.2 Emission model");
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(env.value("start_line").toInt(), 8);
    EXPECT_EQ(env.value("end_line").toInt(), 13);
    EXPECT_EQ(env.value("section_slug").toString().toStdString(),
              "4-2-emission-model");
}

// MD-3 — a nested ### subsection resolves to its own (smaller) range, ending
// at the next same-or-higher heading (the following ##).
TEST(ReadRegionMdSection, SubheadingRange) {
    QTemporaryDir dir;
    const QJsonObject env =
        extractSection(writeDoc(dir, "c.md"), "4-2-1-details");
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(env.value("start_line").toInt(), 11);
    EXPECT_EQ(env.value("end_line").toInt(), 13);
}

// MD-4 — the last section runs to EOF.
TEST(ReadRegionMdSection, LastSectionToEof) {
    QTemporaryDir dir;
    const QJsonObject env = extractSection(writeDoc(dir, "d.md"), "6-cleanup");
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(env.value("start_line").toInt(), 21);
    EXPECT_EQ(env.value("end_line").toInt(), 22);
    EXPECT_TRUE(joinLines(env).contains(QStringLiteral("last section")));
}

// MD-5 — a '#' inside a fenced code block is not a heading boundary; the
// Examples section spans the fence and the trailing prose.
TEST(ReadRegionMdSection, FenceAware) {
    QTemporaryDir dir;
    const QJsonObject env = extractSection(writeDoc(dir, "e.md"), "5-examples");
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(env.value("start_line").toInt(), 14);
    EXPECT_EQ(env.value("end_line").toInt(), 20);  // before "## 6 Cleanup"
    const QString body = joinLines(env);
    EXPECT_TRUE(body.contains(QStringLiteral("# not a heading")));
    EXPECT_TRUE(body.contains(QStringLiteral("Trailing example prose.")));
    EXPECT_FALSE(body.contains(QStringLiteral("## 6 Cleanup")));
}

// MD-6 — unknown slug refuses with section_not_found.
TEST(ReadRegionMdSection, NotFound) {
    QTemporaryDir dir;
    const QJsonObject env = extractSection(writeDoc(dir, "f.md"), "nope-nope");
    EXPECT_FALSE(env.value("ok").toBool());
    EXPECT_EQ(env.value("code").toString().toStdString(), "section_not_found");
}

// MD-7 — selector exclusivity: section + line range, and section + symbol,
// both refuse with bad_args.
TEST(ReadRegionMdSection, SelectorExclusivity) {
    QTemporaryDir dir;
    const QString path = writeDoc(dir, "g.md");
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(kDoc);
        f.close();
    }
    {
        ReadRegion::Options opts;
        opts.section  = QStringLiteral("4-2-emission-model");
        opts.hasLine  = true;
        opts.startLine = 1;
        opts.endLine   = 2;
        const QJsonObject env = ReadRegion::extract(path, opts);
        EXPECT_FALSE(env.value("ok").toBool());
        EXPECT_EQ(env.value("code").toString().toStdString(), "bad_args");
    }
    {
        ReadRegion::Options opts;
        opts.section = QStringLiteral("4-2-emission-model");
        opts.symbol  = QStringLiteral("Foo");
        const QJsonObject env = ReadRegion::extract(path, opts);
        EXPECT_FALSE(env.value("ok").toBool());
        EXPECT_EQ(env.value("code").toString().toStdString(), "bad_args");
    }
}

// MD-9 (ANTS-2234) — a short title resolves a heading carrying a trailing
// parenthetical, when it uniquely prefixes one heading. The echoed
// section_slug is the RESOLVED heading slug, not the input.
TEST(ReadRegionMdSection, ShortTitlePrefixResolvesParenthetical) {
    QTemporaryDir dir;
    const char *doc =
        "# Build Spec\n"
        "## 7. Build order (cheapest-first; independently verifiable)\n"
        "Body for build order.\n"
        "## 8. Cleanup\n"
        "Cleanup body.\n";
    const QJsonObject env =
        extractFrom(writeDoc(dir, "h.md"), doc, "7. Build order");
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(env.value("start_line").toInt(), 2);
    EXPECT_EQ(env.value("end_line").toInt(), 3);  // before "## 8. Cleanup"
    EXPECT_EQ(env.value("section_slug").toString().toStdString(),
              "7-build-order-cheapest-first-independently-verifiable");
    EXPECT_TRUE(joinLines(env).contains(QStringLiteral("Body for build order.")));
}

// MD-10 (ANTS-2234) — a short title that prefixes ≥2 headings refuses with
// section_ambiguous + the candidate slugs, rather than guessing.
TEST(ReadRegionMdSection, AmbiguousPrefixRefuses) {
    QTemporaryDir dir;
    const char *doc =
        "# Amb Spec\n"
        "## 3. Setup (local)\n"
        "Local setup.\n"
        "## 3. Setup (remote)\n"
        "Remote setup.\n";
    const QJsonObject env =
        extractFrom(writeDoc(dir, "i.md"), doc, "3. Setup");
    EXPECT_FALSE(env.value("ok").toBool());
    EXPECT_EQ(env.value("code").toString().toStdString(), "section_ambiguous");
    const QJsonArray cands = env.value("candidates").toArray();
    EXPECT_EQ(cands.size(), 2);
}

// MD-11 (ANTS-2234) — an exact slug still wins even when it ALSO prefixes a
// longer heading; the prefix fallback only fires when no exact match exists.
TEST(ReadRegionMdSection, ExactMatchWinsOverPrefix) {
    QTemporaryDir dir;
    const char *doc =
        "# Spec\n"
        "## 3. Setup\n"
        "Bare setup body.\n"
        "## 3. Setup (extended)\n"
        "Extended setup body.\n";
    const QJsonObject env =
        extractFrom(writeDoc(dir, "j.md"), doc, "3. Setup");
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(env.value("start_line").toInt(), 2);
    EXPECT_EQ(env.value("section_slug").toString().toStdString(), "3-setup");
    EXPECT_TRUE(joinLines(env).contains(QStringLiteral("Bare setup body.")));
}

// MD-12 (ANTS-2234) — the full parenthetical heading text still resolves
// exactly (back-compat: prefix logic must not regress the exact path).
TEST(ReadRegionMdSection, FullParentheticalTextStillResolves) {
    QTemporaryDir dir;
    const char *doc =
        "# Build Spec\n"
        "## 7. Build order (cheapest-first; independently verifiable)\n"
        "Body for build order.\n";
    const QJsonObject env = extractFrom(
        writeDoc(dir, "k.md"), doc,
        "7. Build order (cheapest-first; independently verifiable)");
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(env.value("start_line").toInt(), 2);
    EXPECT_EQ(env.value("section_slug").toString().toStdString(),
              "7-build-order-cheapest-first-independently-verifiable");
}

// MD-8 — handler + schema wiring (the handler needs a live MainWindow).
TEST(ReadRegionMdSection, WiringContract) {
    expect_reset();
    const std::string rc =
        ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    const std::string ci =
        ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(rc, "opts.section") &&
           contains(rc, "QStringLiteral(\"section\")"),
           "MD-8a", "cmdReadRegion does not read the section arg");
    expect(contains(ci, "props[\"section\"]"),
           "MD-8b",
           "read_region inputSchema does not declare the section property");
    EXPECT_EQ(0, expect_failures())
        << expect_failures() << " ANTS-2221 wiring invariant(s) failed";
}

// MD-9 (ANTS-3674) — a heading after an inline span that DEMONSTRATES a fence
// still resolves. CommonMark forbids a backtick in a backtick fence's info
// string, so ```` ```cpp ```` on one line is an inline code span, not an
// opener. The hand-rolled tracker this replaced read it as an opener and went
// blind to every later heading, refusing `section_not_found` on any document
// that teaches fenced code — docs/specs/ANTS-3661.md, live, before the fix.
TEST(ReadRegionMdSection, InlineFenceExampleDoesNotBlindTheScan) {
    QTemporaryDir dir;
    const char *doc =
        "# Spec\n"
        "## 1. Problem\n"
        "Harvest spans (fence-aware, so a\n"
        "```` ```cpp ```` sample is skipped), then keep the span.\n"
        "\n"
        "```\n"
        "a real fenced block\n"
        "```\n"
        "\n"
        "## 5. Out of scope\n"
        "Body for out of scope.\n";
    const QJsonObject env =
        extractFrom(writeDoc(dir, "m.md"), doc, "5. Out of scope");
    ASSERT_TRUE(env.value("ok").toBool())
        << "section_not_found after an inline fence example: "
        << env.value("code").toString().toStdString();
    EXPECT_EQ(env.value("start_line").toInt(), 10);
}

// No row here for the CommonMark closer-LENGTH rule (a 3-backtick line must
// not close a 4-backtick fence). Writing one revealed that
// `MarkdownScan::fenceMask` closes on the fence CHARACTER alone —
// `c == openFence`, no length comparison — so the rule is unimplemented in the
// shared primitive that every markdown consumer in the tree now calls, not in
// this caller. Asserting it here would either fail against correct-for-this-
// layer code or, if written to the current behaviour, freeze the gap. Filed as
// ANTS-3678 against MarkdownScan, where the fix belongs and where one change
// serves all six consumers.
