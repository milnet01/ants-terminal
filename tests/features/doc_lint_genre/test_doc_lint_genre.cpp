// ANTS-5293 — doc_lint genre:"auto": each document's checks come from its own
// `Genre:` line, not from whether it sits under specs_dir. Contract and rows in
// this directory's spec.md.
//
// The engine rows read spec_lint's per-document `lineCount` to tell which
// documents spec_lint ran on: it is filled once per document the checker was
// dispatched to, whatever the document's findings.

#include "doclint.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <gtest/gtest.h>

namespace {

struct Tree {
    QTemporaryDir dir;
    QString       root;
    Tree() : root(QFileInfo(dir.path()).canonicalFilePath()) {}

    void write(const QString &rel, const QString &body) const {
        const QString a = root + QLatin1Char('/') + rel;
        QDir().mkpath(QFileInfo(a).path());
        QFile f(a);
        if (f.open(QIODevice::WriteOnly)) { f.write(body.toUtf8()); f.close(); }
    }
};

DocLint::Options optsFor(const Tree &t, bool genreAuto) {
    DocLint::Options o;
    o.rootCanonical         = t.root;
    o.symbols.rootCanonical = t.root;
    o.specsDirRel           = QStringLiteral("docs/specs");
    o.genreAuto             = genreAuto;
    return o;
}

QSet<QString> setOf(std::initializer_list<const char *> names) {
    QSet<QString> s;
    for (const char *n : names) s.insert(QString::fromLatin1(n));
    return s;
}

}  // namespace

TEST(DocLintGenre, DeclarationShapes) {
    using DocLint::declaredGenre;
    EXPECT_EQ(declaredGenre(QStringLiteral("# T\n\nGenre: spec\n")), QStringLiteral("spec"));
    EXPECT_EQ(declaredGenre(QStringLiteral("**Genre:** `ADR`\n")), QStringLiteral("adr"));
    EXPECT_EQ(declaredGenre(QStringLiteral("- Genre: record\n")), QStringLiteral("record"));
    EXPECT_EQ(declaredGenre(QStringLiteral("  * **Genre** prose  \n")), QStringLiteral("prose"));

    EXPECT_TRUE(declaredGenre(QStringLiteral("Genre: spec and more\n")).isEmpty());
    EXPECT_TRUE(declaredGenre(QStringLiteral("# No declaration\n\nText.\n")).isEmpty());
    EXPECT_TRUE(declaredGenre(QStringLiteral("> Genre: spec\n")).isEmpty());

    // Line 25 is read; line 26 is not.
    const QString pad24 = QString(QStringLiteral("x\n")).repeated(24);
    EXPECT_EQ(declaredGenre(pad24 + QStringLiteral("Genre: plan\n")), QStringLiteral("plan"));
    EXPECT_TRUE(declaredGenre(pad24 + QStringLiteral("x\nGenre: plan\n")).isEmpty());
}

TEST(DocLintGenre, TableMatchesSpec) {
    const QSet<QString> all(DocLint::checkNames().cbegin(), DocLint::checkNames().cend());
    QSet<QString> allButSpec = all;
    allButSpec.remove(QStringLiteral("spec_lint"));

    const auto expect = [](const char *genre, const QSet<QString> &want) {
        const QSet<QString> *got = DocLint::genreChecks(QString::fromLatin1(genre));
        ASSERT_NE(got, nullptr) << genre;
        EXPECT_EQ(*got, want) << genre;
    };
    expect("spec", all);
    expect("plan", allButSpec);
    expect("adr", allButSpec);
    expect("standard", allButSpec);
    expect("prose", setOf({"doc_integrity", "doc_dedup", "doc_facts"}));
    expect("instructions", setOf({"doc_integrity", "doc_citations"}));
    expect("record", setOf({"doc_integrity"}));

    EXPECT_EQ(DocLint::genreChecks(QStringLiteral("poem")), nullptr);
    EXPECT_EQ(DocLint::genreChecks(QString()), nullptr);
}

TEST(DocLintGenre, DeclaredSpecOutsideSpecsDirIsLinted) {
    Tree t;
    t.write(QStringLiteral("docs/design/thing.md"),
            QStringLiteral("# Thing\n\nGenre: spec\n\nBody.\n"));
    const DocLint::Result r = DocLint::run({QStringLiteral("docs/design/thing.md")},
                                           optsFor(t, true));
    EXPECT_TRUE(r.stats.lineCount.contains(QStringLiteral("docs/design/thing.md")));
    EXPECT_TRUE(r.checksRun.contains(QStringLiteral("spec_lint")));
    EXPECT_EQ(r.genres.value(QStringLiteral("docs/design/thing.md")), QStringLiteral("spec"));
}

TEST(DocLintGenre, DeclaredRecordInsideSpecsDirIsNot) {
    Tree t;
    t.write(QStringLiteral("docs/specs/loop-log.md"),
            QStringLiteral("# Loop log\n\nGenre: record\n\nRows.\n"));
    const DocLint::Result r = DocLint::run({QStringLiteral("docs/specs/loop-log.md")},
                                           optsFor(t, true));
    EXPECT_FALSE(r.stats.lineCount.contains(QStringLiteral("docs/specs/loop-log.md")));
    EXPECT_EQ(r.checksRun, QStringList{QStringLiteral("doc_integrity")});
    EXPECT_EQ(r.genres.value(QStringLiteral("docs/specs/loop-log.md")), QStringLiteral("record"));
}

TEST(DocLintGenre, ProseDropsCitationsAndSymbols) {
    Tree t;
    t.write(QStringLiteral("docs/guide.md"),
            QStringLiteral("# Guide\n\nGenre: prose\n\nSee `FooBar::baz` in src/foo.cpp.\n"));
    const DocLint::Result r = DocLint::run({QStringLiteral("docs/guide.md")},
                                           optsFor(t, true));
    const QSet<QString> ran(r.checksRun.cbegin(), r.checksRun.cend());
    EXPECT_EQ(ran, setOf({"doc_integrity", "doc_dedup", "doc_facts"}));
    EXPECT_EQ(r.stats.symbolsTotal, 0);
}

TEST(DocLintGenre, UndeclaredAndUnknownKeepThePathRule) {
    Tree t;
    t.write(QStringLiteral("docs/specs/a.md"), QStringLiteral("# A\n\nNo genre.\n"));
    t.write(QStringLiteral("docs/specs/b.md"), QStringLiteral("# B\n\nGenre: poem\n"));
    t.write(QStringLiteral("docs/c.md"), QStringLiteral("# C\n\nGenre: poem\n"));
    const DocLint::Result r = DocLint::run(
        {QStringLiteral("docs/c.md"), QStringLiteral("docs/specs/a.md"),
         QStringLiteral("docs/specs/b.md")},
        optsFor(t, true));

    EXPECT_TRUE(r.stats.lineCount.contains(QStringLiteral("docs/specs/a.md")));
    EXPECT_TRUE(r.stats.lineCount.contains(QStringLiteral("docs/specs/b.md")));
    EXPECT_FALSE(r.stats.lineCount.contains(QStringLiteral("docs/c.md")));

    EXPECT_TRUE(r.genres.isEmpty());
    EXPECT_EQ(r.unknownGenres.value(QStringLiteral("docs/specs/b.md")), QStringLiteral("poem"));
    EXPECT_EQ(r.unknownGenres.value(QStringLiteral("docs/c.md")), QStringLiteral("poem"));
    EXPECT_FALSE(r.unknownGenres.contains(QStringLiteral("docs/specs/a.md")));
}

TEST(DocLintGenre, PathModeIgnoresTheDeclaration) {
    Tree t;
    t.write(QStringLiteral("docs/design/thing.md"),
            QStringLiteral("# Thing\n\nGenre: spec\n\nBody.\n"));
    t.write(QStringLiteral("docs/specs/log.md"),
            QStringLiteral("# Log\n\nGenre: record\n"));
    const DocLint::Result r = DocLint::run(
        {QStringLiteral("docs/design/thing.md"), QStringLiteral("docs/specs/log.md")},
        optsFor(t, false));
    EXPECT_FALSE(r.stats.lineCount.contains(QStringLiteral("docs/design/thing.md")));
    EXPECT_TRUE(r.stats.lineCount.contains(QStringLiteral("docs/specs/log.md")));
    EXPECT_TRUE(r.genres.isEmpty());
    EXPECT_TRUE(r.unknownGenres.isEmpty());
}
