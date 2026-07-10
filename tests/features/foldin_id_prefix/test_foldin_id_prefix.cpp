// Feature-conformance test for spec.md (ANTS-3473).
//
// The fold-in ID renderers used to hardcode "ANTS-<n>"; a fold-in into a
// project whose roadmap uses a different prefix (e.g. finbreak's
// FIBR-NNNN) got wrong-prefix bullets that collide with the real Ants
// roadmap and break the .roadmap-counter contract. Now the caller sniffs
// the project's dominant `[PREFIX-NNNN]` prefix and threads it in.

#include "roadmapfoldin.h"
#include "coldeyesengine.h"
#include "indiereviewengine.h"
#include "testauditengine.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

namespace {

void writeRoadmap(const QString &dir, const QByteArray &body) {
    QFile f(dir + QStringLiteral("/ROADMAP.md"));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(body);
}

IndieReviewEngine::CorroboratedFinding oneFinding() {
    IndieReviewEngine::CorroboratedFinding f;
    f.file = QStringLiteral("src/foo.cpp");
    f.line = 42;
    f.citingLanes = {QStringLiteral("core")};
    f.title = QStringLiteral("A finding");
    return f;
}

}  // namespace

// INV-1 — sniffIdPrefix returns the dominant bracketed prefix, shrugging
// off a stray token like [UTF-8].
TEST(FoldInIdPrefix, SniffsDominantPrefix) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeRoadmap(tmp.path(),
        "# ROADMAP\n"
        "- [FIBR-0061] one\n"
        "- [FIBR-0062] two\n"
        "- [FIBR-0063] three — encodes [UTF-8] text\n");
    EXPECT_EQ(RoadmapFoldIn::sniffIdPrefix(tmp.path()),
              QStringLiteral("FIBR"));
}

// INV-2 — a roadmap with no counter-style id falls back to the default.
TEST(FoldInIdPrefix, FallsBackWhenNoIds) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeRoadmap(tmp.path(), "# ROADMAP\n- no bracketed ids here\n");
    EXPECT_EQ(RoadmapFoldIn::sniffIdPrefix(tmp.path()),
              QStringLiteral("ANTS"));
    // An absent ROADMAP.md also falls back (custom fallback honoured).
    QTemporaryDir empty;
    ASSERT_TRUE(empty.isValid());
    EXPECT_EQ(RoadmapFoldIn::sniffIdPrefix(empty.path(),
                                           QStringLiteral("ZZ")),
              QStringLiteral("ZZ"));
}

// INV-3 — the cold-eyes fold-in renderer stamps the passed prefix; the
// default keeps the historical "ANTS-" render.
TEST(FoldInIdPrefix, ColdEyesRendersPrefix) {
    const QList<IndieReviewEngine::CorroboratedFinding> fs = {oneFinding()};
    const QList<int> ids = {77};
    const QString custom = ColdEyesEngine::templateColdEyesFoldInBlock(
        fs, ids, QStringLiteral("2026-07-10"), QStringLiteral("FIBR"));
    // ANTS-3480 — zero-padded to min-4 digits, matching op:append.
    EXPECT_TRUE(custom.contains(QStringLiteral("[FIBR-0077]")));
    EXPECT_FALSE(custom.contains(QStringLiteral("[FIBR-77]")));
    EXPECT_FALSE(custom.contains(QStringLiteral("[ANTS-")));
    const QString deflt = ColdEyesEngine::templateColdEyesFoldInBlock(
        fs, ids, QStringLiteral("2026-07-10"));
    EXPECT_TRUE(deflt.contains(QStringLiteral("[ANTS-0077]")));
}

// INV-4 — the indie-review fold-in renderer stamps the passed prefix.
TEST(FoldInIdPrefix, IndieReviewRendersPrefix) {
    const QList<IndieReviewEngine::CorroboratedFinding> fs = {oneFinding()};
    const QList<int> ids = {88};
    const QString custom = IndieReviewEngine::templateIndieReviewFoldInBlock(
        fs, ids, QStringLiteral("2026-07-10"), QStringLiteral("FIBR"));
    // ANTS-3480 — zero-padded to min-4 digits, matching op:append.
    EXPECT_TRUE(custom.contains(QStringLiteral("[FIBR-0088]")));
    EXPECT_FALSE(custom.contains(QStringLiteral("[FIBR-88]")));
    EXPECT_FALSE(custom.contains(QStringLiteral("[ANTS-")));
    const QString deflt = IndieReviewEngine::templateIndieReviewFoldInBlock(
        fs, ids, QStringLiteral("2026-07-10"));
    EXPECT_TRUE(deflt.contains(QStringLiteral("[ANTS-0088]")));
}

// INV-5 (ANTS-3480) — renderId zero-pads the numeric suffix to a minimum of
// four digits, matching roadmap_log op:append; a suffix already ≥4 digits is
// emitted verbatim (no truncation, no over-pad).
TEST(FoldInIdPrefix, RenderIdZeroPadsToFour) {
    EXPECT_EQ(RoadmapFoldIn::renderId(QStringLiteral("FIBR"), 82),
              QStringLiteral("FIBR-0082"));
    EXPECT_EQ(RoadmapFoldIn::renderId(QStringLiteral("ANTS"), 1),
              QStringLiteral("ANTS-0001"));
    // ≥4 digits: untouched (never truncated).
    EXPECT_EQ(RoadmapFoldIn::renderId(QStringLiteral("ANTS"), 3480),
              QStringLiteral("ANTS-3480"));
    EXPECT_EQ(RoadmapFoldIn::renderId(QStringLiteral("ANTS"), 12345),
              QStringLiteral("ANTS-12345"));
}

// INV-6 (ANTS-3498) — isValidIdPrefix accepts the canonical prefix grammar
// (ANTS-3492): 1-16 chars of [A-Za-z0-9_-] containing ≥1 letter. Digit-led is
// fine iff a letter is present; a letter-free or over-long prefix is rejected.
TEST(FoldInIdPrefix, IsValidIdPrefixGrammar) {
    // Accepted.
    EXPECT_TRUE(RoadmapFoldIn::isValidIdPrefix(QStringLiteral("ANTS")));
    EXPECT_TRUE(RoadmapFoldIn::isValidIdPrefix(QStringLiteral("3D_E")));   // digit-led + letter
    EXPECT_TRUE(RoadmapFoldIn::isValidIdPrefix(QStringLiteral("mame-curator")));
    EXPECT_TRUE(RoadmapFoldIn::isValidIdPrefix(QStringLiteral("A")));
    // Rejected.
    EXPECT_FALSE(RoadmapFoldIn::isValidIdPrefix(QString()));               // empty
    EXPECT_FALSE(RoadmapFoldIn::isValidIdPrefix(QStringLiteral("2026")));  // letter-free
    EXPECT_FALSE(RoadmapFoldIn::isValidIdPrefix(QStringLiteral("12-34")));  // letter-free
    EXPECT_FALSE(RoadmapFoldIn::isValidIdPrefix(QStringLiteral("has space")));
    EXPECT_FALSE(RoadmapFoldIn::isValidIdPrefix(
        QStringLiteral("ABCDEFGHIJKLMNOPQR")));                            // 18 chars > 16
}

// INV-7 (ANTS-3498) — TestAuditEngine::foldIn honours an explicit id_prefix
// override (winning over the ROADMAP sniff), and refuses a letter-free one
// with bad_args before any counter touch. Uses dry_run so no ROADMAP write is
// needed; the block still renders for inspection.
TEST(FoldInIdPrefix, TestAuditFoldInHonoursIdPrefixOverride) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    // A FIBR-NNNN roadmap: the sniff would say "FIBR", but the override wins.
    writeRoadmap(tmp.path(),
        "# ROADMAP\n\n## 9.9.9 — active (target: 2026-07)\n\n"
        "- [FIBR-0001] seed\n");

    QJsonObject finding;
    finding["dimension"] = "coverage";
    finding["severity"]  = "MEDIUM";
    finding["file"]      = "tests/foo.cpp";
    finding["line"]      = 10;
    finding["summary"]   = "uncovered branch";

    TestAuditEngine::FoldInRequest req;
    req.callerCwd  = tmp.path();
    req.actionable = QJsonArray{finding};
    req.dryRun     = true;                         // preview, no write
    req.idPrefix   = QStringLiteral("ZOOM");       // explicit override

    const auto r = TestAuditEngine::foldIn(req);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_TRUE(r.block.contains(QStringLiteral("[ZOOM-")))
        << "override must win over the FIBR sniff; got: "
        << r.block.toStdString();
    EXPECT_FALSE(r.block.contains(QStringLiteral("[FIBR-")));

    // A letter-free override is refused with bad_args, no write.
    TestAuditEngine::FoldInRequest bad = req;
    bad.idPrefix = QStringLiteral("2026");
    const auto rb = TestAuditEngine::foldIn(bad);
    EXPECT_FALSE(rb.ok);
    EXPECT_EQ(rb.code, QStringLiteral("bad_args"));
}
