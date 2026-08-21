// ANTS-4547 / ANTS-4550 — wrapped-quotation matching. Behavioural: the
// rule is a pure seam (src/wrapmatch.cpp) so INV-1..5 call it directly,
// INV-6/7 drive cmdRoadmapLogAmendBodyForTest against a seeded ROADMAP,
// and INV-8/9 drive cmdWorkspaceSearch against a seeded tree. See spec.md.

#include "wrapmatch.h"
#include "remotecontrol.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <string>

namespace {

bool writeFile(const QString &path, const QByteArray &bytes) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(bytes) == bytes.size();
}

QByteArray readFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

// ants-v1 seed padded past the 1 KiB minimum-parseable-size gate, with a
// body whose phrases straddle the ~70-col hard wrap.
QByteArray seedRoadmap() {
    return QByteArray(
        "# Test Roadmap\n\n"
        "Intro paragraph that exists purely to pad the file past the\n"
        "1 KiB minimum-parseable-size gate the amend path enforces\n"
        "before it will trust an ants-v1 walk. Lorem ipsum dolor sit\n"
        "amet, consectetur adipiscing elit, sed do eiusmod tempor\n"
        "incididunt ut labore et dolore magna aliqua. Ut enim ad minim\n"
        "veniam, quis nostrud exercitation ullamco laboris nisi ut\n"
        "aliquip ex ea commodo consequat. Duis aute irure dolor in\n"
        "reprehenderit in voluptate velit esse cillum dolore eu fugiat\n"
        "nulla pariatur. Excepteur sint occaecat cupidatat non\n"
        "proident, sunt in culpa qui officia deserunt mollit anim id\n"
        "est laborum. More padding to be safe and clear the gate with\n"
        "comfortable headroom for the parser and the size check above.\n"
        "Sed ut perspiciatis unde omnis iste natus error sit\n"
        "voluptatem accusantium doloremque laudantium, totam rem\n"
        "aperiam, eaque ipsa quae ab illo inventore veritatis et quasi\n"
        "architecto beatae vitae dicta sunt explicabo.\n"
        "\n"
        "## Work Items\n\n"
        "- \xF0\x9F\x93\x8B [ANTS-0042] **Seed bullet headline.**\n"
        "  The value is checked in. And it is not what allocates: the\n"
        "  counter appends against a floor the store owns.\n"
        "  Kind: feature.\n"
        "  Source: seed.\n"
        "\n"
        "- \xF0\x9F\x93\x8B [ANTS-0043] **Twice-wrapped bullet.**\n"
        "  A shared clause that wraps across the\n"
        "  break appears here, and a shared clause that wraps across the\n"
        "  break appears again.\n"
        "  Kind: feature.\n"
        "  Source: seed.\n"
        "\n"
        "- \xF0\x9F\x93\x8B [ANTS-0044] **Column-aligned block bullet.**\n"
        "  The routing table, deliberately aligned:\n"
        "\n"
        "    review-contract    docs/standards/one.md\n"
        "      review-skill     docs/standards/two.md\n"
        "  Kind: feature.\n"
        "  Source: seed.\n"
        "\n");
}

QJsonObject amendReq(const QString &root) {
    QJsonObject o;
    o[QStringLiteral("caller_cwd")] = root;
    o[QStringLiteral("op")]         = QStringLiteral("amend_body");
    return o;
}

}  // namespace

// INV-1 — a needle whose whitespace spans a newline is located, and the
// span is reported in the ORIGINAL text.
TEST(wrapped_quote_match, Inv1FindAcrossHardWrap) {
    const QString hay = QStringLiteral(
        "the value is checked in. And it is not what\nallocates: the counter appends.");
    const auto hits = WrapMatch::find(
        hay, QStringLiteral("not what allocates: the counter"));
    ASSERT_EQ(hits.size(), 1);
    EXPECT_EQ(hay.mid(hits.at(0).start, hits.at(0).length),
              QStringLiteral("not what\nallocates: the counter"))
        << "the span must cover the ORIGINAL text, newline included";
}

// INV-2 — two-sided: a needle carrying its own newlines, indentation and
// `>` blockquote markers matches text that has none.
TEST(wrapped_quote_match, Inv2NormalisationIsTwoSided) {
    const QString flat =
        QStringLiteral("a review gate dismisses a finding whose quote is absent");
    // Pasted straight out of a hard-wrapped blockquote.
    const QString quoted = QStringLiteral(
        "dismisses a finding\n> whose quote");
    EXPECT_EQ(WrapMatch::find(flat, quoted).size(), 1)
        << "a quotation pasted with its own markers must still match";
    // And the mirror: an unmarked needle against a blockquoted haystack.
    const QString blockquoted = QStringLiteral(
        "> a review gate dismisses a finding\n>   whose quote is absent");
    EXPECT_EQ(WrapMatch::find(blockquoted,
                              QStringLiteral("a finding whose quote")).size(), 1);
}

// INV-3 — every occurrence is reported; an empty / whitespace-only needle
// finds nothing rather than everything.
TEST(wrapped_quote_match, Inv3AllOccurrencesAndEmptyNeedle) {
    const QString hay = QStringLiteral("one two\nthree ... one two three");
    EXPECT_EQ(WrapMatch::find(hay, QStringLiteral("one two three")).size(), 2);
    EXPECT_EQ(WrapMatch::find(hay, QStringLiteral("one two three"), 1).size(), 1)
        << "maxHits must stop the scan";
    EXPECT_TRUE(WrapMatch::find(hay, QString()).isEmpty());
    EXPECT_TRUE(WrapMatch::find(hay, QStringLiteral("  \n ")).isEmpty());
}

// INV-4 — the needle is literal: a regex metacharacter matches itself.
TEST(wrapped_quote_match, Inv4NeedleIsLiteral) {
    EXPECT_TRUE(WrapMatch::find(QStringLiteral("axb"),
                                QStringLiteral("a.b")).isEmpty())
        << "`.` must not match an arbitrary character";
    EXPECT_EQ(WrapMatch::find(QStringLiteral("a.b"),
                              QStringLiteral("a.b")).size(), 1);
    // A metacharacter either side of the wrap-tolerant separator.
    EXPECT_EQ(WrapMatch::find(QStringLiteral("cost (x)\nplus (y) each"),
                              QStringLiteral("(x) plus (y)")).size(), 1);
}

// INV-5 — toRegex() drives an external matcher across the break, and an
// empty needle yields an empty string (a refusal, not match-everything).
TEST(wrapped_quote_match, Inv5ToRegexCrossesTheBreak) {
    const QString re = WrapMatch::toRegex(QStringLiteral("quick brown fox"));
    ASSERT_FALSE(re.isEmpty());
    const QRegularExpression rx(re);
    ASSERT_TRUE(rx.isValid()) << re.toStdString();
    EXPECT_TRUE(rx.match(QStringLiteral("the quick\n>  brown fox")).hasMatch());
    EXPECT_TRUE(rx.match(QStringLiteral("the quick brown fox")).hasMatch());
    EXPECT_FALSE(rx.match(QStringLiteral("quick brown cat")).hasMatch());
    EXPECT_TRUE(WrapMatch::toRegex(QString()).isEmpty());
    EXPECT_TRUE(WrapMatch::toRegex(QStringLiteral(" \n ")).isEmpty());
}

// INV-6 — op:"amend_body" amends a phrase spanning a hard-wrapped break in
// ONE call. Reproduces the finbreak FIBR-0288 refusal (ANTS-4550).
TEST(wrapped_quote_match, Inv6AmendBodyAcrossHardWrap) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = QDir(tmp.path()).filePath(QStringLiteral("ROADMAP.md"));
    ASSERT_TRUE(writeFile(path, seedRoadmap()));

    RemoteControl rc(nullptr);
    QJsonObject r = amendReq(tmp.path());
    r[QStringLiteral("id")]       = QStringLiteral("ANTS-0042");
    r[QStringLiteral("old_text")] =
        QStringLiteral("not what allocates: the counter appends");
    r[QStringLiteral("new_text")] =
        QStringLiteral("not what allocates: the store owns the floor and appends");
    const QJsonObject resp = rc.cmdRoadmapLogAmendBodyForTest(r).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << "a wrapped old_text must amend in one call";
    EXPECT_TRUE(resp.value(QStringLiteral("wrapped_match")).toBool())
        << "the envelope must say the match spanned a line break";
    const std::string md = readFile(path).toStdString();
    EXPECT_TRUE(has(md, "the store owns the floor and appends"));
    EXPECT_FALSE(has(md, "allocates: the counter appends"));
    // The bullet is still a bullet: headline, trailer and the sibling
    // below all survive the re-flow.
    EXPECT_TRUE(has(md, "[ANTS-0042] **Seed bullet headline.**"));
    EXPECT_TRUE(has(md, "  Kind: feature."));
    EXPECT_TRUE(has(md, "[ANTS-0043] **Twice-wrapped bullet.**"));
}

// INV-7 — the uniqueness guard survives the normalisation.
TEST(wrapped_quote_match, Inv7WrappedAmbiguityStillRefuses) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = QDir(tmp.path()).filePath(QStringLiteral("ROADMAP.md"));
    ASSERT_TRUE(writeFile(path, seedRoadmap()));

    RemoteControl rc(nullptr);
    QJsonObject r = amendReq(tmp.path());
    r[QStringLiteral("id")]       = QStringLiteral("ANTS-0043");
    // Both occurrences straddle a break, so only the wrapped pass sees
    // them at all — the exact pass counts zero.
    r[QStringLiteral("old_text")] =
        QStringLiteral("clause that wraps across the break appears");
    r[QStringLiteral("new_text")] = QStringLiteral("x");
    const QJsonObject resp = rc.cmdRoadmapLogAmendBodyForTest(r).object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("body_match_ambiguous"));
    EXPECT_TRUE(has(readFile(path).toStdString(),
                    "  A shared clause that wraps across the\n"))
        << "an ambiguous match must leave the file untouched";
}

// INV-10 — the pure seam declines a wrapped match whose span crosses lines of
// DIFFERING indentation (ANTS-4612). The wrapped pass exists for a HARD WRAP,
// where every line of the span shares one indent. Where they differ the block
// is a deliberate structure — a column-aligned table, a nested list — and
// re-flowing it into one line destroys the structure whatever is done with the
// leading whitespace. So the rule is about what the span IS, not about
// repairing the damage afterwards.
TEST(wrapped_quote_match, Inv10DeclinesAcrossDifferingIndent) {
    const QString block =
        QStringLiteral("    review-contract    docs/standards/one.md\n"
                       "      review-skill     docs/standards/two.md\n");
    const WrapMatch::Patch p = WrapMatch::patchOnce(
        block, QStringLiteral("one.md review-skill"), QStringLiteral("x"),
        WrapMatch::Indent::MatchLineIndent);

    EXPECT_TRUE(p.structuredBlock)
        << "INV-10: a span crossing a column-aligned line is not a hard wrap";
    EXPECT_TRUE(p.text.isEmpty())
        << "INV-10: a declined patch must not half-apply";
}

// INV-10 guard — differing indentation is NOT the discriminator, and this is
// the shape that proves it: a bullet's continuation sits deeper than its marker
// line (the hanging indent) and is still an ordinary hard wrap. The naive
// "decline on differing indent" rule broke exactly this, caught by ANTS-3467's
// own case. The signal is structure on the continuation line, not its depth.
TEST(wrapped_quote_match, Inv10HangingIndentStillPatches) {
    const QString para =
        QStringLiteral("  - **Auto-lock timeout:** make it\n"
                       "    user-configurable so users tune it\n");
    const WrapMatch::Patch p = WrapMatch::patchOnce(
        para, QStringLiteral("make it user-configurable"),
        QStringLiteral("expose it in Settings"),
        WrapMatch::Indent::MatchLineIndent);

    EXPECT_FALSE(p.structuredBlock)
        << "INV-10: a hanging indent is a hard wrap, not a structured block";
    EXPECT_EQ(p.hits, 1);
    EXPECT_TRUE(p.wrapped);
    EXPECT_TRUE(p.text.contains(QStringLiteral("expose it in Settings")));
}

// INV-10 — the second structural signal: the span crossed into a NEW list
// item, so the two lines are siblings rather than one wrapped sentence, and
// re-flowing them merges two bullets into one.
TEST(wrapped_quote_match, Inv10DeclinesAcrossAListBoundary) {
    const QString list =
        QStringLiteral("  - first item ends here\n"
                       "  - second item starts here\n");
    const WrapMatch::Patch p = WrapMatch::patchOnce(
        list, QStringLiteral("ends here - second"), QStringLiteral("x"),
        WrapMatch::Indent::MatchLineIndent);

    EXPECT_TRUE(p.structuredBlock)
        << "INV-10: crossing a list boundary is not a hard wrap";
    EXPECT_TRUE(p.text.isEmpty());
}

// INV-11 — end to end, and the part that made this expensive: the damage was
// CUMULATIVE. Every repair attempt triggered the same pass and added more, and
// on a store-backed project hand-repair is reverted by the next render, so
// amend_body was both the only route back and the thing causing the damage.
// Measured on CFG-0196: one row went 4 -> 6 -> 8 -> 12 leading spaces over
// three calls, each returning ok:true with the CORRECT text echoed back.
TEST(wrapped_quote_match, Inv11AmendRefusesToReflowAnAlignedBlock) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = QDir(tmp.path()).filePath(QStringLiteral("ROADMAP.md"));
    ASSERT_TRUE(writeFile(path, seedRoadmap()));
    const std::string before = readFile(path).toStdString();

    RemoteControl rc(nullptr);
    QJsonObject r = amendReq(tmp.path());
    r[QStringLiteral("id")]       = QStringLiteral("ANTS-0044");
    r[QStringLiteral("old_text")] =
        QStringLiteral("one.md review-skill");
    r[QStringLiteral("new_text")] = QStringLiteral("one.md review-agent-rules");
    const QJsonObject resp = rc.cmdRoadmapLogAmendBodyForTest(r).object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool())
        << "INV-11: re-flowing a column-aligned block must be refused";
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("body_match_wrapped_block"))
        << "INV-11: the caller needs a code it can branch on — wrapped_match:true "
           "is not enough, it is also true on the benign case";
    EXPECT_EQ(readFile(path).toStdString(), before)
        << "INV-11: a refused amend must leave the file byte-identical";
}

// INV-11 guard — the ordinary hard-wrapped amend still lands, through the full
// verb rather than the seam alone. INV-6 covers the same path; this asserts the
// new refusal did not swallow it.
TEST(wrapped_quote_match, Inv11HardWrappedAmendStillLands) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = QDir(tmp.path()).filePath(QStringLiteral("ROADMAP.md"));
    ASSERT_TRUE(writeFile(path, seedRoadmap()));

    RemoteControl rc(nullptr);
    QJsonObject r = amendReq(tmp.path());
    r[QStringLiteral("id")]       = QStringLiteral("ANTS-0042");
    r[QStringLiteral("old_text")] =
        QStringLiteral("not what allocates: the counter appends");
    r[QStringLiteral("new_text")] = QStringLiteral("not what allocates: the store appends");
    const QJsonObject resp = rc.cmdRoadmapLogAmendBodyForTest(r).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << "INV-11 guard: a real hard wrap must still amend";
    EXPECT_TRUE(has(readFile(path).toStdString(), "the store appends"));
}

// INV-7 tail — the store path runs the same seam rather than its own copy.
// (The store branch needs a registered project, which this bundle cannot
// seed; the shared call is pinned structurally so the two cannot diverge.)
TEST(wrapped_quote_match, Inv7StorePathSharesTheSeam) {
    const std::string src = ants_test::slurpRemoteControl();
    ASSERT_FALSE(src.empty());
    EXPECT_TRUE(has(src, "WrapMatch::patchOnce"))
        << "both amend_body paths must go through the shared seam";
}

// INV-8 — workspace_search match_wrapped:true finds a quotation spanning a
// hard wrap and reports the line the span STARTS on; off by default the
// same search still misses (ANTS-4547).
TEST(wrapped_quote_match, Inv8WorkspaceSearchMatchWrapped) {
    if (QStandardPaths::findExecutable(QStringLiteral("rg")).isEmpty())
        GTEST_SKIP() << "ripgrep not installed";
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(QDir(tmp.path()).filePath(QStringLiteral("doc.md")),
        QByteArray(
            "# Doc\n"
            "\n"
            "A review gate that dismisses a finding whose quotation\n"
            "cannot be located will ship the real defect.\n")));

    RemoteControl rc(nullptr);
    QJsonObject r;
    r[QStringLiteral("caller_cwd")] = tmp.path();
    r[QStringLiteral("pattern")] =
        QStringLiteral("whose quotation cannot be located");

    const QJsonObject off = rc.cmdWorkspaceSearch(r).object();
    ASSERT_TRUE(off.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(off).toJson().toStdString();
    EXPECT_EQ(off.value(QStringLiteral("matches")).toArray().size(), 0)
        << "line-oriented search must still miss a wrapped quotation";

    r[QStringLiteral("match_wrapped")] = true;
    const QJsonObject on = rc.cmdWorkspaceSearch(r).object();
    ASSERT_TRUE(on.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(on).toJson().toStdString();
    const QJsonArray m = on.value(QStringLiteral("matches")).toArray();
    ASSERT_EQ(m.size(), 1);
    EXPECT_EQ(m.at(0).toObject().value(QStringLiteral("file")).toString(),
              QStringLiteral("doc.md"));
    EXPECT_EQ(m.at(0).toObject().value(QStringLiteral("line")).toInt(), 3)
        << "the reported line is where the matched span STARTS";
    EXPECT_FALSE(m.at(0).toObject().value(QStringLiteral("text"))
                     .toString().contains(QLatin1Char('\n')))
        << "a row's text stays one line";
}

// INV-9 — match_wrapped is literal-mode only.
TEST(wrapped_quote_match, Inv9MatchWrappedRefusesRegex) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    RemoteControl rc(nullptr);
    QJsonObject r;
    r[QStringLiteral("caller_cwd")]    = tmp.path();
    r[QStringLiteral("pattern")]       = QStringLiteral("a|b");
    r[QStringLiteral("regex")]         = true;
    r[QStringLiteral("match_wrapped")] = true;
    const QJsonObject resp = rc.cmdWorkspaceSearch(r).object();
    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_args"));
}
