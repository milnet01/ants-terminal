// Feature-conformance test for ANTS-3689 — doc_symbols gains doc_citations'
// `only=` row filter. Behavioural throughout: docSymbolsBuildResponse is
// pure and public, so every invariant here drives the real builder rather
// than scraping it. INV-6 scrapes the verb layer, which needs a live
// MainWindow to reach. See spec.md.

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

// Three resolved, two unresolved, one not_checked — so every class is
// non-empty and each filter has something to withhold.
QVector<DocSymbols::Symbol> fixture() {
    using R = DocSymbols::Resolution;
    QVector<DocSymbols::Symbol> v;
    const struct { const char *name; R res; } rows[] = {
        {"alphaFn",  R::Resolved},
        {"betaFn",   R::Resolved},
        {"gammaFn",  R::Resolved},
        {"deltaFn",  R::Unresolved},
        {"epsilonFn", R::Unresolved},
        {"zetaFn",   R::NotChecked},
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

QJsonObject build(const QString &only) {
    return RemoteControl::docSymbolsBuildResponse(
        fixture(), {}, /*truncated=*/false,
        QStringList{QStringLiteral("docs/x.md")}, only);
}

int rowsWithResolution(const QJsonObject &o, const char *want) {
    int n = 0;
    for (const auto v : o.value(QStringLiteral("symbols")).toArray())
        if (v.toObject().value(QStringLiteral("resolution")).toString()
            == QString::fromUtf8(want)) ++n;
    return n;
}

int rowCount(const QJsonObject &o) {
    return o.value(QStringLiteral("symbols")).toArray().size();
}

QJsonObject counts(const QJsonObject &o) {
    return o.value(QStringLiteral("counts")).toObject();
}

}  // namespace

// INV-1 — omitting `only` emits every row and says so.
TEST(DocSymbolsOnlyFilter, Inv1DefaultEmitsEveryRow) {
    expect_reset();
    const QJsonObject o = build(QString());
    expect(rowCount(o) == 6, "INV-1: every row emitted by default");
    expect(o.value(QStringLiteral("only")).toString() == QLatin1String("all"),
           "INV-1: the applied value is echoed as \"all\"");
    expect(o.value(QStringLiteral("symbols_filtered_out")).toInt(-1) == 0,
           "INV-1: nothing withheld by default");
    EXPECT_EQ(0, expect_finish());
}

// INV-2 / INV-4 — `unresolved` narrows the rows and reports the withheld.
TEST(DocSymbolsOnlyFilter, Inv24UnresolvedNarrowsAndReportsWithheld) {
    expect_reset();
    const QJsonObject o = build(QStringLiteral("unresolved"));
    expect(rowCount(o) == 2, "INV-2: only the unresolved rows are emitted");
    expect(rowsWithResolution(o, "resolved") == 0,
           "INV-2: no resolved row survives the filter");
    expect(rowsWithResolution(o, "not_checked") == 0,
           "INV-2: no not_checked row survives the filter");
    expect(o.value(QStringLiteral("symbols_filtered_out")).toInt(-1) == 4,
           "INV-4: the withheld count is the rows the filter dropped");
    expect(rowCount(o)
               + o.value(QStringLiteral("symbols_filtered_out")).toInt()
           == counts(o).value(QStringLiteral("total")).toInt(),
           "INV-4: emitted + withheld == counts.total");
    EXPECT_EQ(0, expect_finish());
}

// INV-3 — counts answer what the scan found, not what this call printed.
TEST(DocSymbolsOnlyFilter, Inv3CountsAreWholeDocumentUnderEveryValue) {
    expect_reset();
    const QJsonObject base = counts(build(QString()));
    expect(base.value(QStringLiteral("total")).toInt() == 6
               && base.value(QStringLiteral("resolved")).toInt() == 3
               && base.value(QStringLiteral("unresolved")).toInt() == 2
               && base.value(QStringLiteral("not_checked")).toInt() == 1,
           "INV-3 precondition: the fixture's classes are as intended");
    for (const char *only : {"all", "unresolved", "not_checked"})
        expect(counts(build(QString::fromUtf8(only))) == base,
               "INV-3: counts identical under every accepted value", only);
    EXPECT_EQ(0, expect_finish());
}

// INV-5 — `not_checked` selects that class alone.
TEST(DocSymbolsOnlyFilter, Inv5NotCheckedSelectsThatClass) {
    expect_reset();
    const QJsonObject o = build(QStringLiteral("not_checked"));
    expect(rowCount(o) == 1, "INV-5: one not_checked row emitted");
    expect(rowsWithResolution(o, "not_checked") == 1,
           "INV-5: the emitted row is the not_checked one");
    expect(o.value(QStringLiteral("symbols_filtered_out")).toInt(-1) == 5,
           "INV-5: the other five are withheld");
    EXPECT_EQ(0, expect_finish());
}

// INV-6 — an unrecognised value refuses rather than meaning "all". The
// handler needs a live MainWindow, so the refusal wiring is scraped.
TEST(DocSymbolsOnlyFilter, Inv6UnknownValueRefused) {
    expect_reset();
    const std::string rc = ants_test::slurpRemoteControl();
    expect(rc.find("ANTS-3689-INV-6") != std::string::npos,
           "INV-6: the refusal site carries its anchor");
    EXPECT_EQ(0, expect_finish());
}
