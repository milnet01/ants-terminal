// ANTS-3681 — the structural-bound helpers replacing fixed-byte scrape
// windows. INV-4 is the one worth having: a helper that returned the tail on
// a missing end anchor would restore the very failure it replaces, and would
// do it silently.

#include "../../_support/srcgrep.h"

#include <fstream>
#include <string>

#include <QTemporaryDir>

#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

namespace {

// Two registration entries bounded by the same opening token, which is the
// shape the converted call sites actually use.
const char *kSrc =
    "prologue\n"
    "registerToolProvider(\"alpha\", CallerCwdContract::Required);\n"
    "registerToolProvider(\"beta\", CallerCwdContract::Optional);\n"
    "epilogue\n";

}  // namespace

TEST(SrcgrepRegionBounds, Inv1And2BoundedByTheNearestEndAnchor) {
    const std::string r = ants_test::regionBetween(
        kSrc, "registerToolProvider(\"alpha\"", "registerToolProvider(");
    EXPECT_NE(r.find("Required"), std::string::npos)
        << "the entry's own text is inside the region";
    EXPECT_EQ(r.find("Optional"), std::string::npos)
        << "INV-2: the NEXT entry is not, or one block absorbs its sibling";
    EXPECT_EQ(r.find("epilogue"), std::string::npos);
}

TEST(SrcgrepRegionBounds, Inv3MissingStartAnchorIsEmpty) {
    EXPECT_TRUE(ants_test::regionBetween(kSrc, "nosuchanchor", "registerToolProvider(")
                    .empty());
}

// INV-4 — the load-bearing one. A missing end anchor must NOT yield the tail:
// a caller asserting an absence over a region that quietly became the whole
// file passes because it stopped being a region.
TEST(SrcgrepRegionBounds, Inv4MissingEndAnchorIsEmptyNotTheTail) {
    const std::string r =
        ants_test::regionBetween(kSrc, "registerToolProvider(\"alpha\"",
                                 "nosuchterminator");
    EXPECT_TRUE(r.empty())
        << "returned a tail of " << r.size() << " bytes instead of failing";
}

// INV-5 — bounding a block by its own opening token works, because the end
// search starts past the start anchor rather than at it.
TEST(SrcgrepRegionBounds, Inv5EndAnchorMayRepeatTheStartToken) {
    const std::string r = ants_test::regionBetween(
        kSrc, "registerToolProvider(", "registerToolProvider(");
    EXPECT_NE(r.find("alpha"), std::string::npos);
    EXPECT_EQ(r.find("beta"), std::string::npos)
        << "a zero-length or whole-file region would fail one of these";
}

// The function-body helper, for the same reason: its failure mode is what the
// converted call sites rely on when a signature is renamed.
TEST(SrcgrepRegionBounds, SlurpFunctionBodyIsBraceMatchedAndFailsEmpty) {
    const std::string src =
        "void Other::before() { int a = 0; }\n"
        "void Klass::target() {\n"
        "    if (a) { nested(); }\n"
        "    tail();\n"
        "}\n"
        "void Other::after() { poison(); }\n";
    const std::string b = ants_test::slurpFunctionBody(src, "Klass::target");
    EXPECT_NE(b.find("nested()"), std::string::npos) << "nesting is spanned";
    EXPECT_NE(b.find("tail()"), std::string::npos)   << "to the real close";
    EXPECT_EQ(b.find("poison()"), std::string::npos)
        << "and stops there, or the region absorbs the next function";
    EXPECT_TRUE(ants_test::slurpFunctionBody(src, "Klass::absent").empty());
}

// INV-6 (ANTS-5634) — a source list naming a path that cannot be opened fails
// the calling test. Why this exists: the readers appended "" for such a path
// and said nothing, so an "X is absent" scrape passed over part of a class.
TEST(SrcgrepRegionBounds, Inv6ListReaderReportsAnUnreadablePath) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    static std::string real, empty, bogus;
    real = dir.filePath(QStringLiteral("a.cpp")).toStdString();
    empty = dir.filePath(QStringLiteral("empty.cpp")).toStdString();
    bogus = dir.filePath(QStringLiteral("no_such_file.cpp")).toStdString();
    { std::ofstream(real) << "int a;"; }
    { std::ofstream give(empty); }

    // Every path opens, one of them empty: the text, and no failure.
    EXPECT_EQ(ants_test::slurpSourceList((real + ";" + empty).c_str()),
              "int a;\n");

    // One path does not open: a non-fatal failure that names it.
    static std::string list;
    list = real + ";" + bogus;
    EXPECT_NONFATAL_FAILURE(ants_test::slurpSourceList(list.c_str()),
                            "no_such_file.cpp");
}
