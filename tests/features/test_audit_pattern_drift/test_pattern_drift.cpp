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
