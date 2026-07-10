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

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
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
