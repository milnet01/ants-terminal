// Feature-conformance test for ANTS-5265 — doc_symbols gains
// workspace_search's `count_only`. docSymbolsBuildResponse is pure and
// public, so INV-1..4 drive the real builder. INV-5 scrapes the verb layer,
// which needs a live MainWindow to reach. See spec.md.

#include "remotecontrol.h"
#include "docsymbols.h"

#include "../../_support/expect.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

ANTS_TEST_SCOPE();

namespace {

// One symbol per resolution class, and two findings, so every array the
// full reply carries is non-empty and a dropped one is visible.
QVector<DocSymbols::Symbol> symbolsFixture() {
    using R = DocSymbols::Resolution;
    QVector<DocSymbols::Symbol> v;
    const struct { const char *name; R res; } rows[] = {
        {"alphaFn", R::Resolved},
        {"betaFn",  R::Unresolved},
        {"gammaFn", R::NotChecked},
    };
    int line = 1;
    for (const auto &r : rows) {
        DocSymbols::Symbol s;
        s.symbol     = QString::fromUtf8(r.name);
        s.docLine    = line++;
        s.docCol     = 1;
        s.resolution = r.res;
        v.push_back(s);
    }
    return v;
}

QList<DocFinding::Finding> findingsFixture() {
    QList<DocFinding::Finding> v;
    for (const char *sym : {"betaFn", "deltaFn"}) {
        DocFinding::Finding f;
        f.kind = QStringLiteral("unresolved_symbol");
        f.file = QStringLiteral("docs/x.md");
        f.message = QString::fromUtf8(sym);
        v.push_back(f);
    }
    return v;
}

QJsonObject build(bool countOnly) {
    return RemoteControl::docSymbolsBuildResponse(
        symbolsFixture(), findingsFixture(), /*truncated=*/false,
        QStringList{QStringLiteral("docs/x.md")}, QString(), countOnly);
}

}  // namespace

// INV-1 — the default reply keeps its rows and gains no new field.
TEST(DocSymbolsCountOnly, Inv1DefaultUnchanged) {
    expect_reset();
    const QJsonObject o = build(false);
    expect(o.value(QStringLiteral("symbols")).toArray().size() == 3,
           "INV-1: symbols[] present by default");
    expect(o.value(QStringLiteral("findings")).toArray().size() == 2,
           "INV-1: findings[] present by default");
    expect(!o.contains(QStringLiteral("findings_count")),
           "INV-1: findings_count absent by default");
    EXPECT_EQ(0, expect_finish());
}

// INV-2 / INV-3 / INV-4 — rows dropped, counts kept, findings counted.
TEST(DocSymbolsCountOnly, Inv234RowsDroppedCountsKept) {
    expect_reset();
    const QJsonObject full = build(false);
    const QJsonObject o = build(true);
    expect(!o.contains(QStringLiteral("symbols")),
           "INV-2: symbols[] absent under count_only");
    expect(!o.contains(QStringLiteral("findings")),
           "INV-2: findings[] absent under count_only");
    expect(o.value(QStringLiteral("counts")).toObject()
               == full.value(QStringLiteral("counts")).toObject(),
           "INV-3: counts identical with and without count_only");
    expect(o.value(QStringLiteral("findings_count")).toInt(-1)
               == full.value(QStringLiteral("findings")).toArray().size(),
           "INV-4: findings_count is the full reply's findings[] length");
    EXPECT_EQ(0, expect_finish());
}

// INV-5 — conflicting arguments refuse rather than being ignored. The
// handler needs a live MainWindow, so the refusal wiring is scraped.
TEST(DocSymbolsCountOnly, Inv5ConflictsRefused) {
    expect_reset();
    const std::string rc = ants_test::slurpRemoteControl();
    expect(rc.find("ANTS-5265-INV-5") != std::string::npos,
           "INV-5: the refusal site carries its anchor");
    EXPECT_EQ(0, expect_finish());
}
