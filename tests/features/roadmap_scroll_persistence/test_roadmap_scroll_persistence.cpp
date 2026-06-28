// Feature-conformance test for tests/features/roadmap_scroll_persistence/spec.md.
//
// Locks ANTS-1264 — Roadmap dialog scroll-position persistence (spec
// ANTS-1154 §4.5 / INV-13). Drives the pure resolver
// RoadmapDialog::resolveScrollAnchor against synthetic present-id /
// present-slug sets (the deterministic contract), and source-greps
// roadmapdialog.cpp for the capture-on-close / restore-on-show wiring.

#include "roadmapdialog.h"

#include <QSet>
#include <QString>

#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

namespace {

using Anchor = RoadmapDialog::ScrollAnchor;
using Target = RoadmapDialog::ScrollTarget;

Anchor anchor(const QString &slug, const QString &id, int offset) {
    Anchor a;
    a.sectionSlug = slug;
    a.id = id;
    a.offsetPx = offset;
    return a;
}

// INV-1 — saved id present → Card with the saved offset preserved.
TEST(RoadmapScrollPersistence, Inv1CardHit) {
    const QSet<QString> ids{QStringLiteral("ANTS-9002"),
                            QStringLiteral("ANTS-9003")};
    const QSet<QString> slugs{QStringLiteral("features")};
    const Target t = RoadmapDialog::resolveScrollAnchor(
        anchor(QStringLiteral("features"), QStringLiteral("ANTS-9002"), 37),
        ids, slugs);
    EXPECT_EQ(t.kind, Target::Card);
    EXPECT_EQ(t.id, QStringLiteral("ANTS-9002"));
    EXPECT_EQ(t.offsetPx, 37);
}

// INV-2 — saved id gone but its section survives → Section (offset dropped).
TEST(RoadmapScrollPersistence, Inv2SectionFallback) {
    const QSet<QString> ids{QStringLiteral("ANTS-9003")};       // 9002 deleted
    const QSet<QString> slugs{QStringLiteral("features")};
    const Target t = RoadmapDialog::resolveScrollAnchor(
        anchor(QStringLiteral("features"), QStringLiteral("ANTS-9002"), 37),
        ids, slugs);
    EXPECT_EQ(t.kind, Target::Section);
    EXPECT_EQ(t.sectionSlug, QStringLiteral("features"));
}

// INV-3 — neither id nor section survives → Top.
TEST(RoadmapScrollPersistence, Inv3TopFallback) {
    const QSet<QString> ids{QStringLiteral("ANTS-9003")};
    const QSet<QString> slugs{QStringLiteral("other-section")};
    const Target t = RoadmapDialog::resolveScrollAnchor(
        anchor(QStringLiteral("features"), QStringLiteral("ANTS-9002"), 37),
        ids, slugs);
    EXPECT_EQ(t.kind, Target::Top);
}

// INV-4 — id and section both present → Card wins.
TEST(RoadmapScrollPersistence, Inv4CardPrecedence) {
    const QSet<QString> ids{QStringLiteral("ANTS-9002")};
    const QSet<QString> slugs{QStringLiteral("features")};
    const Target t = RoadmapDialog::resolveScrollAnchor(
        anchor(QStringLiteral("features"), QStringLiteral("ANTS-9002"), 5),
        ids, slugs);
    EXPECT_EQ(t.kind, Target::Card);
    EXPECT_EQ(t.id, QStringLiteral("ANTS-9002"));
}

// INV-5 — empty id + empty slug → Top, even if the present sets happen to
// carry an empty string (empty fields must never count as a hit).
TEST(RoadmapScrollPersistence, Inv5EmptyAnchorIsTop) {
    const QSet<QString> ids{QString()};
    const QSet<QString> slugs{QString()};
    const Target t = RoadmapDialog::resolveScrollAnchor(
        anchor(QString(), QString(), 0), ids, slugs);
    EXPECT_EQ(t.kind, Target::Top);
}

// INV-6 — capture-on-close + restore-on-show wiring, round-tripping through
// Config::roadmapScrollAnchors. Source-grep so a future refactor that drops
// either half trips the test rather than silently disabling the feature.
TEST(RoadmapScrollPersistence, Inv6SourceWiring) {
    const std::string src = ants_test::slurpFile(ROADMAPDIALOG_CPP);
    ASSERT_FALSE(src.empty()) << "could not read roadmapdialog.cpp";
    auto has = [&](const char *needle) {
        return src.find(needle) != std::string::npos;
    };
    EXPECT_TRUE(has("captureScrollAnchor()"))
        << "closeEvent must capture the scroll anchor";
    EXPECT_TRUE(has("restoreScrollAnchor()"))
        << "showEvent must restore the scroll anchor";
    EXPECT_TRUE(has("void RoadmapDialog::showEvent"))
        << "a showEvent override is required for first-show restore";
    EXPECT_TRUE(has("setRoadmapScrollAnchors"))
        << "capture must persist through Config::setRoadmapScrollAnchors";
    EXPECT_TRUE(has("roadmapScrollAnchors()"))
        << "restore must read Config::roadmapScrollAnchors";
}

}  // namespace
