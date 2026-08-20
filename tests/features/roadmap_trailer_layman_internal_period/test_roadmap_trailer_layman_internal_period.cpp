// ANTS-4596 — feature-conformance test: a `Layman:` value must not be
// truncated at a full stop INSIDE it. Pre-fix rxLayman() captured with a
// non-greedy `(.+?)` closed by `[\.\n]`, so `(e.g. …)` stored `(e` and a
// decimal `41.5` stored `41`. The character class exists to strip the
// TRAILING period (ANTS-1154 INV-4); stopping at the first one is a
// different rule that only diverges when the value carries an internal dot.
//
// The two neighbouring keys already do this correctly — ANTS-3764 for
// `Source:`, ANTS-3382 for `Evidence:` — so the shape asserted here is
// theirs, not a new one.
//
// Drives the pure static parseBullets.

#include "../../_support/expect.h"
#include "roadmapdialog.h"

#include <gtest/gtest.h>

#include <QString>
#include <QStringLiteral>
#include <QVector>

#include <string>

ANTS_TEST_SCOPE();

namespace {

// 📋 = U+1F4CB (F0 9F 93 8B).
const char *kSeed =
    "# Roadmap\n\n"
    "## Work\n\n"
    // INV-1 — an abbreviation inside a parenthetical.
    "- \xF0\x9F\x93\x8B [ANTS-2000] **Abbreviation.**\n"
    "  **Layman:** When you have two Claude tabs open (e.g. Ants Terminal + "
    "Vestige), the status bar shows the wrong tab.\n"
    "  Kind: fix.\n"
    // INV-2 — a decimal, and a sub-numbered decimal.
    "- \xF0\x9F\x93\x8B [ANTS-2001] **Decimal.**\n"
    "  **Layman:** Sub-numbered items like 41.5 and 41.5.B are wrongly "
    "merged into one.\n"
    "  Kind: fix.\n"
    // INV-3 — the trailing stop is still removed.
    "- \xF0\x9F\x93\x8B [ANTS-2002] **Plain sentence.**\n"
    "  **Layman:** A plain sentence with no abbreviation in it.\n"
    "  Kind: fix.\n"
    // INV-4 — a second key on the same line still ends the value.
    "- \xF0\x9F\x93\x8B [ANTS-2003] **Two keys one line.**\n"
    "  Layman: the value ends here. Kind: chore.\n"
    // INV-5 — ANTS-4542's wrap continuation still applies to this key.
    "- \xF0\x9F\x93\x8B [ANTS-2004] **Wrapped value.**\n"
    "  Layman: a value that is hard-wrapped mid-phrase (two reports, same\n"
    "  day) is rejoined.\n"
    "  Kind: fix.\n"
    // INV-6 — a file extension inside the value survives.
    "- \xF0\x9F\x93\x8B [ANTS-2005] **Dotted token.**\n"
    "  **Layman:** Get the .deb version building on the build server.\n"
    "  Kind: package.\n";

QString laymanOf(const QString &id) {
    const auto recs = RoadmapDialog::parseBullets(QString::fromUtf8(kSeed));
    for (const auto &r : recs)
        if (r.id == id)
            return r.layman;
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

// INV-1 — the parenthetical survives whole, brackets balanced. Pre-fix this
// stored "When you have two Claude tabs open (e".
TEST(roadmap_trailer_layman_internal_period, Inv1AbbreviationInsideValue) {
    const QString v = laymanOf(QStringLiteral("ANTS-2000"));
    EXPECT_EQ(v, QStringLiteral(
        "When you have two Claude tabs open (e.g. Ants Terminal + Vestige), "
        "the status bar shows the wrong tab"));
    EXPECT_EQ(v.count(QLatin1Char('(')), v.count(QLatin1Char(')')))
        << "unbalanced bracket means the value was cut: " << v.toStdString();
}

// INV-2 — every digit of a decimal is kept. Pre-fix: "Sub-numbered items
// like 41".
TEST(roadmap_trailer_layman_internal_period, Inv2DecimalInsideValue) {
    EXPECT_EQ(laymanOf(QStringLiteral("ANTS-2001")), QStringLiteral(
        "Sub-numbered items like 41.5 and 41.5.B are wrongly merged into one"));
}

// INV-3 — ANTS-1154 INV-4: exactly one trailing stop goes, and the stored
// value carries no terminal punctuation. This is the case the character
// class was written for and it must not regress.
TEST(roadmap_trailer_layman_internal_period, Inv3TrailingStopStillStripped) {
    const QString v = laymanOf(QStringLiteral("ANTS-2002"));
    EXPECT_EQ(v, QStringLiteral("A plain sentence with no abbreviation in it"));
    EXPECT_FALSE(v.endsWith(QLatin1Char('.')));
}

// INV-4 — the value stops at a following declaration on the same line, and
// that declaration is still read. Pre-fix the first-period stop gave this
// for free; an end-of-line capture has to do it on purpose, so this is the
// regression the fix is most likely to cause.
TEST(roadmap_trailer_layman_internal_period, Inv4SecondKeyOnTheSameLineEndsIt) {
    EXPECT_EQ(laymanOf(QStringLiteral("ANTS-2003")),
              QStringLiteral("the value ends here"));
    EXPECT_EQ(kindOf(QStringLiteral("ANTS-2003")), QStringLiteral("chore"));
}

// INV-5 — ANTS-4542's continuation still reaches this key.
TEST(roadmap_trailer_layman_internal_period, Inv5WrappedValueStillRejoined) {
    EXPECT_EQ(laymanOf(QStringLiteral("ANTS-2004")), QStringLiteral(
        "a value that is hard-wrapped mid-phrase (two reports, same day) "
        "is rejoined"));
}

// INV-6 — a file extension survives, the guarantee ANTS-3382 states for
// Evidence: and ANTS-4542 INV-5 for Source:.
TEST(roadmap_trailer_layman_internal_period, Inv6FileExtensionSurvives) {
    EXPECT_EQ(laymanOf(QStringLiteral("ANTS-2005")), QStringLiteral(
        "Get the .deb version building on the build server"));
}

}  // namespace
