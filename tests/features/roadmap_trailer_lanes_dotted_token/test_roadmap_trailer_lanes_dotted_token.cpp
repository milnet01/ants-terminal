// ANTS-4597 — feature-conformance test: a `Lanes:` value must not be
// truncated at a full stop INSIDE it. Pre-fix rxLanes() captured with a
// non-greedy `(.+?)` closed by `[\.\n]` — the shape ANTS-4596 removed from
// rxLayman() — so the list ended at the first dot, which for this key is
// usually a dot inside a filename.
//
// The loss is worse here than for the layman key: splitTrailerList() sees a
// shorter string and yields FEWER LANES, and a short list terminated by a
// full stop reads as a correct declaration. Measured over the machine-global
// store 2026-08-20: 24 bodies write a dotted token inside an inline `Lanes:`
// run, and zero stored lane names contain a dot.
//
// The shape asserted here is ANTS-3764's for `Source:`, ANTS-3382's for
// `Evidence:` and ANTS-4596's for `Layman:` — not a new one. What is new is
// that the stop and the chop land BEFORE splitTrailerList(), which is the one
// way this key differs from the three that already have the fix.
//
// Drives the pure static parseBullets.

#include "../../_support/expect.h"
#include "roadmapdialog.h"

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>
#include <QStringLiteral>
#include <QVector>

#include <string>

ANTS_TEST_SCOPE();

namespace {

// 📋 = U+1F4CB (F0 9F 93 8B).
const char *kSeed =
    "# Roadmap\n\n"
    "## Work\n\n"
    // INV-1 — ANTS-1117's real declaration: five lanes, the first dotted.
    "- \xF0\x9F\x93\x8B [ANTS-3000] **Members after a dotted lane.**\n"
    "  Kind: fix. Lanes: remotecontrol.cpp, AuditDialog, RoadmapDialog, "
    "MainWindow, ClaudeChrome.\n"
    // INV-2 — ANTS-1444's real declaration: the extension is part of the lane.
    "- \xF0\x9F\x93\x8B [ANTS-3001] **Extension survives.**\n"
    "  Lanes: ants_audit_lib, CMakeLists.txt.\n"
    // INV-3 — the trailing stop is still removed from the last member.
    "- \xF0\x9F\x93\x8B [ANTS-3002] **Plain list.**\n"
    "  Lanes: chrome, tests.\n"
    // INV-4 — a following declaration on the same line still ends the list.
    // `Source:` is the key rxTrailerKey() omits, so reusing that set here
    // would run the lane list straight through it.
    "- \xF0\x9F\x93\x8B [ANTS-3003] **Following Source declaration.**\n"
    "  Lanes: packaging, docs. Source: regression-2026-08-20.\n"
    "- \xF0\x9F\x93\x8B [ANTS-3004] **Following Kind declaration.**\n"
    "  Lanes: core, vt. Kind: chore.\n"
    // INV-5 — ANTS-4542's wrap continuation still applies to this key.
    "- \xF0\x9F\x93\x8B [ANTS-3005] **Wrapped list.**\n"
    "  Lanes: roadmapparse, remotecontrol_roadmap_query,\n"
    "  roadmaprender.\n"
    "  Kind: fix.\n"
    // INV-7 — a declaration followed by ORDINARY PROSE on the same line, which
    // is ANTS-2143's real body. The value ends at its sentence stop, so the
    // prose is not split into lanes.
    "- \xF0\x9F\x93\x8B [ANTS-3007] **Prose after the declaration.**\n"
    "  Lanes: hooks. Verify the resulting predicate table still matches "
    "docs/specs/ANTS-2141.md after the dedupe.\n"
    // INV-6 — ANTS-1125's real declaration: a LEADING dot, mid-list.
    "- \xF0\x9F\x93\x8B [ANTS-3006] **Leading-dot lane.**\n"
    "  Lanes: RoadmapDialog, docs/standards, .claude/bump.json, "
    "packaging/rotate.sh.\n";

QStringList lanesOf(const QString &id) {
    const auto recs = RoadmapDialog::parseBullets(QString::fromUtf8(kSeed));
    for (const auto &r : recs)
        if (r.id == id)
            return r.lanes;
    ADD_FAILURE() << "no bullet with id " << id.toStdString();
    return {};
}

QString kindOf(const QString &id) {
    const auto recs = RoadmapDialog::parseBullets(QString::fromUtf8(kSeed));
    for (const auto &r : recs)
        if (r.id == id)
            return r.kind;
    ADD_FAILURE() << "no bullet with id " << id.toStdString();
    return {};
}

QString sourceOf(const QString &id) {
    const auto recs = RoadmapDialog::parseBullets(QString::fromUtf8(kSeed));
    for (const auto &r : recs)
        if (r.id == id)
            return r.source;
    ADD_FAILURE() << "no bullet with id " << id.toStdString();
    return {};
}

// INV-1 — every member after the dotted one survives. Pre-fix this stored
// the single lane ["remotecontrol"], losing four of five.
TEST(roadmap_trailer_lanes_dotted_token, Inv1MembersAfterADottedLane) {
    const QStringList lanes = lanesOf(QStringLiteral("ANTS-3000"));
    EXPECT_EQ(lanes, (QStringList{QStringLiteral("remotecontrol.cpp"),
                                  QStringLiteral("AuditDialog"),
                                  QStringLiteral("RoadmapDialog"),
                                  QStringLiteral("MainWindow"),
                                  QStringLiteral("ClaudeChrome")}))
        << "lost members at the dot: " << lanes.join(QLatin1Char('|')).toStdString();
    // The `Kind:` written before the list on the same line is unaffected.
    EXPECT_EQ(kindOf(QStringLiteral("ANTS-3000")),
              QStringLiteral("fix"));
}

// INV-2 — the extension is part of the lane, not a sentence stop. Pre-fix
// this stored ["ants_audit_lib", "CMakeLists"], which still names a
// real-looking lane and is therefore the harder half to spot.
TEST(roadmap_trailer_lanes_dotted_token, Inv2FileExtensionSurvives) {
    EXPECT_EQ(lanesOf(QStringLiteral("ANTS-3001")),
              (QStringList{QStringLiteral("ants_audit_lib"),
                           QStringLiteral("CMakeLists.txt")}));
}

// INV-3 — the single trailing full stop is still removed, BEFORE the split,
// so no stored lane carries it. This is what the character class was written
// for and it must not regress.
TEST(roadmap_trailer_lanes_dotted_token, Inv3TrailingStopStillStripped) {
    const QStringList lanes = lanesOf(QStringLiteral("ANTS-3002"));
    EXPECT_EQ(lanes, (QStringList{QStringLiteral("chrome"), QStringLiteral("tests")}));
    for (const QString &l : lanes)
        EXPECT_FALSE(l.endsWith(QLatin1Char('.')))
            << "lane kept the sentence period: " << l.toStdString();
}

// INV-4 — a following declaration on the same line still ends the list, and
// is itself still read. Pre-fix the first-period stop gave this for free; an
// end-of-line capture has to do it on purpose. `Source:` is the case that
// fails if rxTrailerKey() is reused instead of a stop set built for this key.
TEST(roadmap_trailer_lanes_dotted_token, Inv4FollowingSourceEndsTheList) {
    EXPECT_EQ(lanesOf(QStringLiteral("ANTS-3003")),
              (QStringList{QStringLiteral("packaging"), QStringLiteral("docs")}));
    EXPECT_EQ(sourceOf(QStringLiteral("ANTS-3003")),
              QStringLiteral("regression-2026-08-20"));
}

TEST(roadmap_trailer_lanes_dotted_token, Inv4FollowingKindEndsTheList) {
    EXPECT_EQ(lanesOf(QStringLiteral("ANTS-3004")),
              (QStringList{QStringLiteral("core"), QStringLiteral("vt")}));
    EXPECT_EQ(kindOf(QStringLiteral("ANTS-3004")),
              QStringLiteral("chore"));
}

// INV-5 — ANTS-4542's continuation still reaches this key, and the rejoined
// text is split into whole lanes rather than truncated at the wrap.
TEST(roadmap_trailer_lanes_dotted_token, Inv5WrappedListStillRejoined) {
    EXPECT_EQ(lanesOf(QStringLiteral("ANTS-3005")),
              (QStringList{QStringLiteral("roadmapparse"),
                           QStringLiteral("remotecontrol_roadmap_query"),
                           QStringLiteral("roadmaprender")}));
}

// INV-6 — a LEADING dot survives too, and the members after it are kept.
// Pre-fix this stored ["RoadmapDialog", "docs/standards"], cut at the dot
// that opens `.claude`.
TEST(roadmap_trailer_lanes_dotted_token, Inv6LeadingDotLane) {
    EXPECT_EQ(lanesOf(QStringLiteral("ANTS-3006")),
              (QStringList{QStringLiteral("RoadmapDialog"),
                           QStringLiteral("docs/standards"),
                           QStringLiteral(".claude/bump.json"),
                           QStringLiteral("packaging/rotate.sh")}));
}

// INV-7 — the value ends at its SENTENCE stop, not at the end of the line. A
// lane run is one clause, and prose written after it on the same line must not
// be split into lanes: `subsystem` and `indie_review_partition` key on this
// list. Caught against the live store — an end-of-line stop repaired the dotted
// runs and turned this bullet into one lane carrying a whole sentence.
TEST(roadmap_trailer_lanes_dotted_token, Inv7ProseAfterTheDeclarationIsNotALane) {
    const QStringList lanes = lanesOf(QStringLiteral("ANTS-3007"));
    EXPECT_EQ(lanes, (QStringList{QStringLiteral("hooks")}))
        << "absorbed the prose: " << lanes.join(QLatin1Char('|')).toStdString();
}

}  // namespace
