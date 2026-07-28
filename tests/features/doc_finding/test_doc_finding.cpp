// ANTS-3664 — the shared doc-lint finding shape, conformance test.
// See spec.md for the full contract. Pure: a struct in, JSON out, no
// filesystem — except INV-4/INV-5, which are source scrapes.

#include "docfinding.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>

#include <gtest/gtest.h>

#if !defined(SRC_DOCFINDING_H_PATH)
#error "doc_finding test needs the test_core SRC_DOCFINDING_H_PATH compile def"
#endif

using DocFinding::Finding;
using DocFinding::countsByVerbAndKind;
using DocFinding::toJson;

namespace {

QString slurp(const char *path) {
    QFile f(QString::fromUtf8(path));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

Finding make(const QString &verb, const QString &kind, const QString &file,
             int line, const QString &message, bool autoFixable = false) {
    Finding f;
    f.verb = verb;
    f.kind = kind;
    f.file = file;
    f.line = line;
    f.message = message;
    f.autoFixable = autoFixable;
    return f;
}

}  // namespace

// INV-1 — toJson always emits the five value keys, `line` included when it is
// 0, plus auto_fixable only when true, and emission_index on NEITHER.
TEST(DocFinding, Inv1JsonKeysAndOmission) {
    // A default-constructed Finding: line is 0 and the flag is false.
    const QJsonObject bare = toJson(Finding{});
    EXPECT_EQ(bare.size(), 5) << "default Finding must serialise exactly five keys";
    EXPECT_TRUE(bare.contains(QStringLiteral("verb")));
    EXPECT_TRUE(bare.contains(QStringLiteral("kind")));
    EXPECT_TRUE(bare.contains(QStringLiteral("file")));
    EXPECT_TRUE(bare.contains(QStringLiteral("message")));
    // `line` is NOT omit-when-false: 0 means document scope, which is a value.
    // Omitting it would make a document-level finding indistinguishable from a
    // malformed one.
    ASSERT_TRUE(bare.contains(QStringLiteral("line")))
        << "line:0 is document scope and must be emitted, not omitted";
    EXPECT_EQ(bare.value(QStringLiteral("line")).toInt(), 0);
    EXPECT_FALSE(bare.contains(QStringLiteral("auto_fixable")))
        << "auto_fixable is omit-when-false (ANTS-3636 INV-28 convention)";

    // A fully-populated one: six keys, the sixth being auto_fixable.
    Finding full = make(QStringLiteral("doc_integrity"), QStringLiteral("toc_gap"),
                        QStringLiteral("a.md"), 12,
                        QStringLiteral("section '3. Invariants' missing from TOC"),
                        /*autoFixable=*/true);
    full.emissionIndex = 7;  // set by the producer; must not reach the wire
    const QJsonObject rich = toJson(full);
    EXPECT_EQ(rich.size(), 6);
    EXPECT_EQ(rich.value(QStringLiteral("verb")).toString(),
              QStringLiteral("doc_integrity"));
    EXPECT_EQ(rich.value(QStringLiteral("line")).toInt(), 12);
    ASSERT_TRUE(rich.contains(QStringLiteral("auto_fixable")));
    EXPECT_TRUE(rich.value(QStringLiteral("auto_fixable")).toBool());

    // The clause that fails against the obvious serialiser. emissionIndex
    // orders findings WITHIN a run and is unstable across runs (a different
    // checks[] set renumbers identical findings), so it is noise on the wire.
    // ANTS-3669's fixed[] carries {verb, kind, file, line} for the same reason.
    EXPECT_FALSE(bare.contains(QStringLiteral("emission_index")));
    EXPECT_FALSE(rich.contains(QStringLiteral("emission_index")))
        << "emission_index must never be serialised, in any array, by any verb";
}

// INV-1 (list overload) — the array form is the object form, in order.
TEST(DocFinding, Inv1ListOverloadPreservesOrder) {
    const QList<Finding> fs = {
        make(QStringLiteral("a"), QStringLiteral("k1"), QStringLiteral("x.md"), 1,
             QStringLiteral("first")),
        make(QStringLiteral("b"), QStringLiteral("k2"), QStringLiteral("y.md"), 2,
             QStringLiteral("second")),
    };
    const QJsonArray arr = toJson(fs);
    ASSERT_EQ(arr.size(), 2);
    EXPECT_EQ(arr.at(0).toObject().value(QStringLiteral("message")).toString(),
              QStringLiteral("first"));
    EXPECT_EQ(arr.at(1).toObject().value(QStringLiteral("message")).toString(),
              QStringLiteral("second"));
    EXPECT_TRUE(toJson(QList<Finding>{}).isEmpty());
}

// INV-2 — countsByVerbAndKind counts every finding it is handed, nesting verb
// then kind. Asserted by VALUE, not shape: a serialiser emitting every leaf as
// 1 would pass a shape-only assertion.
TEST(DocFinding, Inv2CountsByVerbAndKind) {
    const QList<Finding> fs = {
        make(QStringLiteral("doc_integrity"), QStringLiteral("toc_gap"),
             QStringLiteral("a.md"), 1, QStringLiteral("m")),
        make(QStringLiteral("doc_integrity"), QStringLiteral("toc_gap"),
             QStringLiteral("a.md"), 2, QStringLiteral("m")),
        make(QStringLiteral("doc_integrity"), QStringLiteral("dead_anchor"),
             QStringLiteral("b.md"), 3, QStringLiteral("m")),
        make(QStringLiteral("spec_lint"), QStringLiteral("invariant_no_test"),
             QStringLiteral("c.md"), 4, QStringLiteral("m")),
    };
    const QJsonObject counts = countsByVerbAndKind(fs);
    ASSERT_EQ(counts.size(), 2);

    const QJsonObject integrity = counts.value(QStringLiteral("doc_integrity")).toObject();
    ASSERT_EQ(integrity.size(), 2);
    EXPECT_EQ(integrity.value(QStringLiteral("toc_gap")).toInt(), 2)
        << "a repeated (verb, kind) must accumulate, not overwrite";
    EXPECT_EQ(integrity.value(QStringLiteral("dead_anchor")).toInt(), 1);

    const QJsonObject lint = counts.value(QStringLiteral("spec_lint")).toObject();
    ASSERT_EQ(lint.size(), 1);
    EXPECT_EQ(lint.value(QStringLiteral("invariant_no_test")).toInt(), 1);

    EXPECT_TRUE(countsByVerbAndKind(QList<Finding>{}).isEmpty());
}

// INV-3 — JSON key names are snake_case while the C++ members are camelCase.
// Assert the literal, so renaming the member cannot silently rename the field.
TEST(DocFinding, Inv3WireNamesAreSnakeCase) {
    Finding f;
    f.autoFixable = true;
    const QJsonObject o = toJson(f);
    EXPECT_TRUE(o.contains(QStringLiteral("auto_fixable")));
    EXPECT_FALSE(o.contains(QStringLiteral("autoFixable")))
        << "the camelCase member name must not reach the wire";
}

// INV-4 — src/docfinding.h includes exactly four Qt headers, all QtCore.
// An ALLOW-list, not a deny-list: the point of the invariant is the GUI class
// names nobody thought to enumerate. Set equality, so an extra include fails
// (the leak this exists for) and a missing one also fails (the struct changed
// shape without anyone revisiting the claim).
//
// The compiler is the stronger half: the bundle links ants_core_lib, which
// links Qt6::Core alone, so a GUI include fails the BUILD. This scrape exists
// because a build failure names the wrong culprit — it points at the test.
TEST(DocFinding, Inv4CoreOnlyHeader) {
    const QString src = slurp(SRC_DOCFINDING_H_PATH);
    ASSERT_FALSE(src.isEmpty()) << "cannot read " << SRC_DOCFINDING_H_PATH;

    QSet<QString> found;
    QRegularExpression re(QStringLiteral(R"(^#include\s*<(Q[A-Za-z0-9_]*)>)"),
                          QRegularExpression::MultilineOption);
    auto it = re.globalMatch(src);
    while (it.hasNext()) found.insert(it.next().captured(1));

    const QSet<QString> expected = {
        QStringLiteral("QString"), QStringLiteral("QList"),
        QStringLiteral("QJsonObject"), QStringLiteral("QJsonArray"),
    };
    EXPECT_EQ(found, expected)
        << "docfinding.h's Qt include set changed; a GUI include here is the "
           "leak this invariant exists for";
}

// INV-5 — every checker engine in this family takes its document as text, never
// as a path it opens itself (§ 2.3 rule 1).
//
// This skipped until all three native engines existed, rather than passing
// vacuously: a green scrape over absent files is exactly the false assurance
// the spec warns about. ANTS-3660 landed the last of them (2026-07-28), so it
// now runs.
//
// The skip note predicted `docsymbols.cpp` would need an exemption for its
// source-tree walk. It does not — the walk lives in `SymbolQuery`, so the
// engine itself opens nothing either, and the invariant holds for all three
// with no exception at all. Asserting the stronger form because it is the true
// one; an exemption carried for a file that does not need it is a hole the next
// engine slips through.
TEST(DocFinding, Inv5EnginesTakeTextNotPaths) {
    for (const char *path : {SRC_DOCDEDUP_CPP_PATH, SRC_DOCSYMBOLS_CPP_PATH,
                             SRC_SPECLINT_CPP_PATH}) {
        QFile f(QString::fromUtf8(path));
        ASSERT_TRUE(f.open(QIODevice::ReadOnly))
            << "engine source must be readable: " << path;
        const QString src = QString::fromUtf8(f.readAll());
        f.close();
        ASSERT_FALSE(src.isEmpty());
        for (const char *banned : {"QFile", "QSaveFile", "QTextStream",
                                   "QIODevice", "QDirIterator"})
            EXPECT_FALSE(src.contains(QString::fromUtf8(banned)))
                << path << " must take text, not a path it opens: " << banned;
    }
}
