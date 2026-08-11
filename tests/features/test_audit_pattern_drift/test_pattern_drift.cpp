// Drift guard (ANTS-1450): docs/standards/test-audit-grep-patterns.json
// is the in-tree source of truth for the test_audit_* pre-pass pattern
// set + dimension taxonomy. This test asserts the compiled
// TestAuditEngine tables match it exactly, so the documented resource
// and the engine cannot silently diverge.
//
// The JSON path is injected by CMake as TEST_AUDIT_PATTERNS_JSON.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include "testauditengine.h"

namespace {

QJsonObject loadJson() {
    QFile f(QStringLiteral(TEST_AUDIT_PATTERNS_JSON));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return {};
    return doc.object();
}

}  // namespace

// INV-D1 — dimensions array equals kDimensions() exactly (order included).
TEST(TestAuditPatternDrift, Inv1DimensionsMatchEngine) {
    const QJsonObject root = loadJson();
    ASSERT_FALSE(root.isEmpty()) << "could not load/parse "
                                 << TEST_AUDIT_PATTERNS_JSON;
    const QJsonArray dims = root.value(QStringLiteral("dimensions")).toArray();
    const QStringList engine = TestAuditEngine::kDimensions();
    ASSERT_EQ(dims.size(), engine.size())
        << "JSON dimensions count != kDimensions() count";
    for (int i = 0; i < engine.size(); ++i) {
        EXPECT_EQ(dims.at(i).toString(), engine.at(i))
            << "dimension mismatch at index " << i;
    }
}

// INV-D2 — patterns array matches prePassPatterns() id/dimension/regex
// in order.
TEST(TestAuditPatternDrift, Inv2PatternsMatchEngine) {
    const QJsonObject root = loadJson();
    ASSERT_FALSE(root.isEmpty());
    const QJsonArray pats = root.value(QStringLiteral("patterns")).toArray();
    const auto &engine = TestAuditEngine::prePassPatterns();
    ASSERT_EQ(pats.size(), engine.size())
        << "JSON patterns count != prePassPatterns() count";
    for (int i = 0; i < engine.size(); ++i) {
        const QJsonObject o = pats.at(i).toObject();
        EXPECT_EQ(o.value(QStringLiteral("id")).toString(), engine.at(i).id)
            << "pattern id mismatch at index " << i;
        EXPECT_EQ(o.value(QStringLiteral("dimension")).toString(),
                  engine.at(i).dimension)
            << "pattern dimension mismatch at index " << i
            << " (id=" << engine.at(i).id.toStdString() << ")";
        EXPECT_EQ(o.value(QStringLiteral("regex")).toString(),
                  engine.at(i).regex)
            << "pattern regex mismatch at index " << i
            << " (id=" << engine.at(i).id.toStdString() << ")";
    }
}

// INV-D5 — ANTS-4112: the JSON `languages` gate matches the engine's, per
// pattern and in order. Without this row the gate could be added to one side
// only, and a pattern that silently ungates itself is indistinguishable from
// one that was never gated — which is the shape of the defect it fixes.
TEST(TestAuditPatternDrift, Ants4112LanguageGateMatchesEngine) {
    const QJsonObject root = loadJson();
    ASSERT_FALSE(root.isEmpty());
    const QJsonArray pats = root.value(QStringLiteral("patterns")).toArray();
    const auto &engine = TestAuditEngine::prePassPatterns();
    ASSERT_EQ(pats.size(), engine.size());
    int gated = 0;
    for (int i = 0; i < engine.size(); ++i) {
        const QString j = pats.at(i).toObject()
                              .value(QStringLiteral("languages")).toString();
        EXPECT_EQ(j, engine.at(i).languages)
            << "languages mismatch at index " << i
            << " (id=" << engine.at(i).id.toStdString() << ")";
        if (!engine.at(i).languages.isEmpty()) ++gated;
    }
    // The two the report named, and no silent third: a gate that spreads is a
    // coverage loss nobody would see, since a gated-out pattern reports nothing.
    EXPECT_EQ(gated, 2) << "exactly cpp_exit and system_shell_out are gated";
    for (const auto &p : engine)
        if (p.id == QLatin1String("cpp_exit")
            || p.id == QLatin1String("system_shell_out"))
            EXPECT_EQ(p.languages, QStringLiteral("cpp"));
}

// INV-D6 — ANTS-4111: `default_dimensions` is what dimensions:"auto" resolves
// to, `style_dimensions` is what it leaves out, and the three lists agree on
// both sides. dimensions_active[] is what /test-audit seeds its subagents from,
// so a drift here re-introduces the five dimensions the skill deliberately cut.
TEST(TestAuditPatternDrift, Ants4111DefaultDimensionsMatchEngine) {
    const QJsonObject root = loadJson();
    ASSERT_FALSE(root.isEmpty());

    const auto asList = [&root](const char *key) {
        QStringList out;
        for (const auto &v : root.value(QLatin1String(key)).toArray())
            out << v.toString();
        return out;
    };
    EXPECT_EQ(asList("default_dimensions"), TestAuditEngine::kDefaultDimensions());
    EXPECT_EQ(asList("style_dimensions"), TestAuditEngine::kStyleDimensions());

    // Internal consistency: default + style partitions the full taxonomy, and
    // the style five are still ACCEPTED — only no longer default.
    const QStringList all = TestAuditEngine::kDimensions();
    const QStringList def = TestAuditEngine::kDefaultDimensions();
    const QStringList sty = TestAuditEngine::kStyleDimensions();
    EXPECT_EQ(def.size() + sty.size(), all.size());
    for (const QString &d : sty) {
        EXPECT_TRUE(all.contains(d)) << d.toStdString() << " must stay accepted";
        EXPECT_FALSE(def.contains(d)) << d.toStdString() << " must not be default";
    }
    for (const QString &d : def) EXPECT_TRUE(all.contains(d));
}

// INV-D3 — every pattern dimension is a member of the dimensions list.
TEST(TestAuditPatternDrift, Inv3PatternDimensionsAreKnown) {
    const QJsonObject root = loadJson();
    ASSERT_FALSE(root.isEmpty());
    QSet<QString> dimSet;
    const QJsonArray dims = root.value(QStringLiteral("dimensions")).toArray();
    for (const auto &d : dims) dimSet.insert(d.toString());
    const QJsonArray pats = root.value(QStringLiteral("patterns")).toArray();
    for (const auto &p : pats) {
        const QString dim = p.toObject().value(QStringLiteral("dimension")).toString();
        EXPECT_TRUE(dimSet.contains(dim))
            << "pattern dimension '" << dim.toStdString()
            << "' not in dimensions list";
    }
}

// INV-D4 — pattern ids are unique.
TEST(TestAuditPatternDrift, Inv4PatternIdsUnique) {
    const QJsonObject root = loadJson();
    ASSERT_FALSE(root.isEmpty());
    const QJsonArray pats = root.value(QStringLiteral("patterns")).toArray();
    QSet<QString> seen;
    for (const auto &p : pats) {
        const QString id = p.toObject().value(QStringLiteral("id")).toString();
        EXPECT_FALSE(seen.contains(id))
            << "duplicate pattern id '" << id.toStdString() << "'";
        seen.insert(id);
    }
}
