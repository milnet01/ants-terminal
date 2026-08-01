// Feature-conformance test for ANTS-3764 step 2 — the five BulletRecord
// fields ANTS-3757 § 2.3 needs. Contract:
// tests/features/roadmap_parse_widening/spec.md
//
// Behavioural, against the reader in ants_core_lib. Each fixture writes its
// line numbers out in the comment column so INV-3 can assert exact spans
// rather than a relation any monotonic numbering would satisfy.

#include <gtest/gtest.h>

#include "passheadingwrite.h"
#include "roadmapparse.h"

#include <QString>

namespace {

// ants-v1. The `Source:` line carries an internal period (`rpmlint.log`) AND
// a following `Lanes:` key — the two corpus shapes INV-2 was measured
// against — and the second bullet's leading slot holds an off-grammar id
// while its prose cites a grammar-conforming one.
QString antsV1Doc() {
    return QStringLiteral(
        "## Active\n"                                                    //  1
        "\n"                                                             //  2
        "- 📋 [ANTS-1234] **First item.**\n"                             //  3
        "  Kind: refactor.\n"                                            //  4
        "  Source: in-session-2026-07-31 (rpmlint.log, first build). "   //  5
        "Lanes: docs.\n"
        "\n"                                                             //  6
        "- ✅ [Cl9] **Off-grammar id.**\n"                               //  7
        "  Refers to [ANTS-9999] in prose.\n"                            //  8
        "\n"                                                             //  9
        "- 🚧 **No id here.**\n"                                         // 10
        "  Kind: fix.\n");                                               // 11
}

// pass-headings. Two Pass headings + two Status markers is the format
// sniffer's 2+2 threshold. Pass 44.1's Status value is the corpus's
// `**un-gated (2026-07-05).**`, asterisks and all.
QString passDoc() {
    return QStringLiteral(
        "## Passes\n"                                                    //  1
        "\n"                                                             //  2
        "#### Pass 43.5.B (CRITICAL, S) Sub-pass thing\n"                //  3
        "\n"                                                             //  4
        "- **Status**: done (v3.20.0, 2026-07-05). Adds catalogs.\n"     //  5
        "- **Finding**: something.\n"                                    //  6
        "\n"                                                             //  7
        "#### Pass 44.1 (LOW, S) Ungated thing\n"                        //  8
        "- **Status**: **un-gated (2026-07-05).**\n"                     //  9
        "\n");                                                           // 10
}

}  // namespace

// INV-1 — sourceStatus is the verbatim Status value, qualifier tail and
// asterisks intact; empty where status comes from a marker instead.
TEST(roadmap_parse_widening, Inv1SourceStatusIsVerbatim) {
    const auto passes = RoadmapParse::parseBullets(passDoc());
    ASSERT_EQ(passes.size(), 2);
    EXPECT_EQ(passes[0].sourceStatus,
              QStringLiteral("done (v3.20.0, 2026-07-05). Adds catalogs."))
        << "INV-1: the whole remainder of the line, not the matched word";
    EXPECT_EQ(passes[0].status, QStringLiteral("✅"))
        << "INV-1: the emoji mapping is unchanged by the widening";
    EXPECT_EQ(passes[1].sourceStatus,
              QStringLiteral("**un-gated (2026-07-05).**"))
        << "INV-1: matching strips the leading `*`; storage strips nothing";

    const auto v1 = RoadmapParse::parseBullets(antsV1Doc());
    ASSERT_EQ(v1.size(), 3);
    for (const auto &rec : v1)
        EXPECT_TRUE(rec.sourceStatus.isEmpty())
            << "INV-1: an emoji bullet carries no Status line";
}

// INV-2 — source survives an internal period and stops at a trailer key.
TEST(roadmap_parse_widening, Inv2SourceValue) {
    const auto v1 = RoadmapParse::parseBullets(antsV1Doc());
    ASSERT_EQ(v1.size(), 3);
    EXPECT_EQ(v1[0].source,
              QStringLiteral("in-session-2026-07-31 (rpmlint.log, first build)"))
        << "INV-2: to end-of-line (the period inside rpmlint.log survives), "
           "cut at the following Lanes: key, one trailing period dropped";
    EXPECT_EQ(v1[0].kind, QStringLiteral("refactor"))
        << "INV-2: the sibling Kind: parse is untouched";
    EXPECT_TRUE(v1[1].source.isEmpty())
        << "INV-2: empty when the bullet carries no Source: line";

    // The bold label form. 24 lines in the corpus write `**Source:**`, and
    // rxLayman already tolerates the same shape for its own key.
    const auto bold = RoadmapParse::parseBullets(QStringLiteral(
        "## Active\n"
        "- 📋 [ANTS-1] **Bold label.**\n"
        "  **Source:** in-session-2026-07-31.\n"));
    ASSERT_EQ(bold.size(), 1);
    EXPECT_EQ(bold[0].source, QStringLiteral("in-session-2026-07-31"))
        << "INV-2: the label's closing `**` is markup, not the value";
}

// INV-3 — 1-based inclusive spans, trailing blanks excluded, no overlap.
TEST(roadmap_parse_widening, Inv3LineSpans) {
    const auto v1 = RoadmapParse::parseBullets(antsV1Doc());
    ASSERT_EQ(v1.size(), 3);
    EXPECT_EQ(v1[0].firstLine, 3);
    EXPECT_EQ(v1[0].lastLine, 5);
    EXPECT_EQ(v1[1].firstLine, 7);
    EXPECT_EQ(v1[1].lastLine, 8);
    EXPECT_EQ(v1[2].firstLine, 10);
    EXPECT_EQ(v1[2].lastLine, 11);

    const auto passes = RoadmapParse::parseBullets(passDoc());
    ASSERT_EQ(passes.size(), 2);
    EXPECT_EQ(passes[0].firstLine, 3) << "INV-3: the heading line itself";
    EXPECT_EQ(passes[0].lastLine, 6)
        << "INV-3: the blank line 7 is outside the span";
    EXPECT_EQ(passes[1].firstLine, 8);
    EXPECT_EQ(passes[1].lastLine, 9);

    // The property ANTS-3757 INV-11 partitions on: spans are ordered and
    // disjoint, so no source line is claimed by two records.
    for (int i = 1; i < v1.size(); ++i)
        EXPECT_GT(v1[i].firstLine, v1[i - 1].lastLine)
            << "INV-3: record " << i << " overlaps its predecessor";
}

// INV-4 — the designator survives, and reader and writer agree on the id.
TEST(roadmap_parse_widening, Inv4PassDesignator) {
    const auto passes = RoadmapParse::parseBullets(passDoc());
    ASSERT_EQ(passes.size(), 2);
    EXPECT_EQ(passes[0].passDesignator, QStringLiteral("43.5.B"))
        << "INV-4: the sub-pass suffix is part of the designator";
    EXPECT_EQ(passes[1].passDesignator, QStringLiteral("44.1"));
    // ANTS-3757 INV-10, asserted rather than assumed.
    for (const auto &rec : passes)
        EXPECT_EQ(PassHeadingWrite::passIdFromDesignator(rec.passDesignator),
                  rec.id)
            << "INV-4: writer and reader disagree on the synthesised id";

    const auto v1 = RoadmapParse::parseBullets(antsV1Doc());
    for (const auto &rec : v1)
        EXPECT_TRUE(rec.passDesignator.isEmpty())
            << "INV-4: empty off the pass path";
}

// INV-5 — the leading-slot token as written, before any acceptance test.
TEST(roadmap_parse_widening, Inv5IdTokenAsWritten) {
    const auto v1 = RoadmapParse::parseBullets(antsV1Doc());
    ASSERT_EQ(v1.size(), 3);
    EXPECT_EQ(v1[0].idToken, QStringLiteral("ANTS-1234"))
        << "INV-5: brackets are markdown, not part of the id";
    EXPECT_EQ(v1[1].idToken, QStringLiteral("Cl9"))
        << "INV-5: an off-grammar token reaches migration to be quarantined";
    // The positional half, measured rather than assumed: this bullet's
    // leading slot reads [Cl9] and its prose cites [ANTS-9999], and `id`
    // reports the citation. roadmap-data-model.md § 7.1 recognises an id
    // only at the leading position, so `id` cannot stand in for this field
    // even where it is non-empty.
    EXPECT_EQ(v1[1].id, QStringLiteral("ANTS-9999"))
        << "INV-5: `id` is positionless — that is the point of `idToken`";
    EXPECT_TRUE(v1[2].idToken.isEmpty())
        << "INV-5: empty when the leading slot holds no token";

    // The case that proves the field changes an OUTCOME and not just an
    // implementation. `[ANTS-119&]` is not a fixture: it is on 7 bullets of
    // this project's own ROADMAP.md, and neither the strict body-wide `rxId`
    // nor ANTS-1987's `rxLeadBracketId` accepts the `&`. So `id` is empty —
    // the item reads as id-less, and roadmap-data-model.md § 7.2 would issue
    // it a SECOND identity for an item that visibly carries one. With the
    // token in hand, § 2.6 quarantines it instead and the report names it.
    const auto malformed = RoadmapParse::parseBullets(QStringLiteral(
        "## Active\n"
        "- ✅ [ANTS-119&] **Malformed id in the leading slot.**\n"));
    ASSERT_EQ(malformed.size(), 1);
    EXPECT_EQ(malformed[0].idToken, QStringLiteral("ANTS-119&"))
        << "INV-5: verbatim, including what makes it off-grammar";
    EXPECT_TRUE(malformed[0].id.isEmpty())
        << "INV-5: the reader still rejects it — which is why the raw token "
           "has to be carried separately";

    // A leading-slot token followed by `(` or `:` is a markdown link.
    const auto links = RoadmapParse::parseBullets(QStringLiteral(
        "## Links\n"
        "- 📋 [Doc](path.md) — do the thing.\n"
        "- ✅ [ref]: https://example.invalid\n"));
    ASSERT_EQ(links.size(), 2);
    for (const auto &rec : links)
        EXPECT_TRUE(rec.idToken.isEmpty())
            << "INV-5: a markdown link in the leading slot is not an id; got "
            << rec.idToken.toStdString();

    const auto passes = RoadmapParse::parseBullets(passDoc());
    for (const auto &rec : passes)
        EXPECT_TRUE(rec.idToken.isEmpty())
            << "INV-5: a pass id is synthesised, never written in source";
}

// ANTS-3773 — in a github-task-list roadmap the leading bold run is an id for
// some bullets and prose for others, and the branch asserted the convention
// without testing it. Measured over the corpus 2026-08-01: one project writes
// this shape, and there it is HALF true — 288 real ids (MT1, FW W5, Audit X1)
// against 166 prose lead-ins (Photo mode, Aerodynamics). Applying the ants-v1
// id-shaped guard would have stripped the real ones, several of which contain
// a space.
//
// The discriminator is a TRAILING COLON and nothing more, and that narrowness
// is deliberate. It is not a heuristic: `idTokenPattern` is
// `[A-Za-z0-9][A-Za-z0-9_-]*-\d+`, so a colon cannot appear in an id at all.
// A wider "must contain a digit" rule fitted the ten roadmaps measured and
// broke ANTS-1438's Vestige fixture, whose ids are `Terrain System` and
// `JustBoldNoSeparator`. Separating a multi-word id from a multi-word prose
// lead-in is not possible from the text, which is ANTS-3771's argument.
TEST(RoadmapParseWidening, GfmBoldLeadInIsAnIdOnlyWhenItLooksLikeOne) {
    const auto recs = RoadmapParse::parseBullets(QStringLiteral(
        "## Phase 9E\n"
        "- [x] **MT1** Real id, the shape this branch exists to respect.\n"
        "- [x] **FW W5** Real id with a space — the ants-v1 guard would lose it.\n"
        "- [x] **Audit X1** Real id, likewise.\n"
        "- [x] **Phase 9E-2:** EventBus bridge — a label, not an id.\n"
        "- [x] **Phase 9E-2:** 12 event nodes — the same label again.\n"
        "- [x] **Terrain System** — digitless and multi-word, and a REAL id: "
        "ANTS-1438's Vestige fixture depends on it.\n"
        "- [x] Plain bullet with no bold run at all.\n"));
    ASSERT_EQ(recs.size(), 7);

    EXPECT_EQ(recs[0].idToken.toStdString(), std::string("MT1"));
    EXPECT_EQ(recs[1].idToken.toStdString(), std::string("FW W5"))
        << "a real id may contain a space — this is why the ants-v1 id-shaped "
           "guard cannot simply be reused here";
    EXPECT_EQ(recs[2].idToken.toStdString(), std::string("Audit X1"));

    // The two that collided. A label ending in ':' introduces prose; without
    // this both bullets carry the id `Phase 9E-2:` and fold to one identity,
    // which fails ANTS-3756's UNIQUE (project_id, id_fold) and refuses the
    // whole project at migration.
    EXPECT_TRUE(recs[3].idToken.isEmpty())
        << "a bold label ending in ':' is not an id; got "
        << recs[3].idToken.toStdString();
    EXPECT_TRUE(recs[4].idToken.isEmpty());
    EXPECT_EQ(recs[5].idToken.toStdString(), std::string("Terrain System"))
        << "a digitless multi-word bold run is still an id — requiring a digit "
           "looked right against the ten roadmaps measured and broke ANTS-1438's "
           "Vestige fixture, which is why the guard is the colon and nothing more";
    EXPECT_TRUE(recs[6].idToken.isEmpty());

    // `id` moves with it, but NOT to empty: ANTS-1428's GFM adapter falls back
    // to a content-hash id when the bullet carries no bold one, and marks it
    // `synthetic`. That is the correct destination for these two — ANTS-3757
    // § 2.9 discards a synthetic GFM id and plans the item id-less, so
    // migration allocates it a real one instead of inheriting prose.
    EXPECT_TRUE(recs[3].synthetic) << "a dropped bold label must fall through to "
                                     "the synthetic id, not keep the label";
    EXPECT_FALSE(recs[5].synthetic);
    EXPECT_EQ(recs[0].id.toStdString(), std::string("MT1"));
    EXPECT_FALSE(recs[0].synthetic) << "a real bold id is not synthetic";
}
