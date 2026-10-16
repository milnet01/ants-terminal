// ANTS-5734 § 3, § 7 — the trace engine against the fixture project. One case
// per ship-1 invariant that trace::serve decides on its own: INV-1 to INV-12,
// INV-14 to INV-16 and INV-18. Each case copies tests/features/trace/fixture/
// into a temporary directory outside any repository and serves from there:
// inside this repository the spec's own text would match the fixture names.
// Contract: docs/specs/ANTS-5734-trace-verb.md.

#include "trace/extract.h"
#include "trace/trace.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QStringList>
#include <QTemporaryDir>

#include <functional>
#include <optional>

#ifndef TRACE_FIXTURE_DIR
#error "test_trace_serve needs TRACE_FIXTURE_DIR"
#endif

namespace {

// Copies the fixture, dot-directories included, into a fresh temporary
// directory and points XDG_DATA_HOME at an empty one, so a query file in the
// user's own data directory cannot change an answer.
class TraceServe : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(m_project.isValid());
        ASSERT_TRUE(m_data.isValid());
        const QString src = QStringLiteral(TRACE_FIXTURE_DIR);
        QDirIterator it(src, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
        int copied = 0;
        while (it.hasNext()) {
            const QString from = it.next();
            const QString to = m_project.path() + from.mid(src.size());
            QDir().mkpath(QFileInfo(to).absolutePath());
            ASSERT_TRUE(QFile::copy(from, to)) << to.toStdString();
            ++copied;
        }
        ASSERT_GT(copied, 0) << "fixture not found at " << src.toStdString();
        m_root = QFileInfo(m_project.path()).canonicalFilePath();
        m_savedXdg = qgetenv("XDG_DATA_HOME");
        m_hadXdg = qEnvironmentVariableIsSet("XDG_DATA_HOME");
        qputenv("XDG_DATA_HOME", m_data.path().toUtf8());
    }
    void TearDown() override {
        if (m_hadXdg) qputenv("XDG_DATA_HOME", m_savedXdg);
        else qunsetenv("XDG_DATA_HOME");
    }

    QJsonObject call(const QJsonObject &args) const { return trace::serve(args, m_root); }
    QJsonObject traceSymbol(const QString &symbol) const {
        return call({{QStringLiteral("symbol"), symbol}});
    }
    bool write(const QString &rel, const QByteArray &text) const {
        const QString p = m_root + QLatin1Char('/') + rel;
        QDir().mkpath(QFileInfo(p).absolutePath());
        QFile f(p);
        return f.open(QIODevice::WriteOnly) && f.write(text) == text.size();
    }

    QTemporaryDir m_project;
    QTemporaryDir m_data;
    QString m_root;
    QByteArray m_savedXdg;
    bool m_hadXdg = false;
};

QJsonArray arr(const QJsonObject &o, const char *key) {
    return o.value(QLatin1String(key)).toArray();
}

std::string dump(const QJsonValue &v) {
    return (v.isObject() ? QJsonDocument(v.toObject()) : QJsonDocument(v.toArray()))
        .toJson(QJsonDocument::Compact)
        .toStdString();
}

QJsonArray rowsWhere(const QJsonArray &rows, const std::function<bool(const QJsonObject &)> &keep) {
    QJsonArray out;
    for (const QJsonValue &v : rows)
        if (keep(v.toObject())) out.append(v);
    return out;
}

std::optional<QJsonObject> notCovered(const QJsonObject &reply, const QString &reason) {
    for (const QJsonValue &v : arr(reply, "not_covered"))
        if (v.toObject().value(QStringLiteral("reason")).toString() == reason)
            return v.toObject();
    return std::nullopt;
}

// Every row of a reply, in section order, as one sequence (§ 2.2's paging
// order). `tests` is the one section whose rows sit one level down.
QStringList flatRows(const QJsonObject &reply) {
    static const char *const sections[] = {"definitions", "callers", "references", "tests",
                                           "docs", "spec_invariants", "roadmap_items"};
    QStringList out;
    for (const char *s : sections) {
        QJsonValue v = reply.value(QLatin1String(s));
        if (v.isObject()) v = v.toObject().value(QStringLiteral("files"));
        for (const QJsonValue &r : v.toArray())
            out << QString::fromLatin1(s) + QLatin1Char(' ') +
                       (r.isString() ? r.toString()
                                     : QString::fromUtf8(QJsonDocument(r.toObject())
                                                             .toJson(QJsonDocument::Compact)));
    }
    return out;
}

}  // namespace

// INV-1 — exactly one of symbol and path; a symbol outside § 2.1's shape.
TEST_F(TraceServe, Inv1RefusesNeitherBothAndMalformedSymbol) {
    for (const QJsonObject &args :
         {QJsonObject{},
          QJsonObject{{QStringLiteral("symbol"), QStringLiteral("spanFn")},
                      {QStringLiteral("path"), QStringLiteral("span.cpp")}},
          QJsonObject{{QStringLiteral("symbol"), QStringLiteral("a b")}}}) {
        const QJsonObject r = call(args);
        SCOPED_TRACE(dump(args));
        EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool(true));
        EXPECT_EQ(r.value(QStringLiteral("code")).toString(), QStringLiteral("bad_args"));
    }
}

// INV-2 — a definition spans its whole node, not its name.
TEST_F(TraceServe, Inv2DefinitionSpansWholeNode) {
    const QJsonObject r = traceSymbol(QStringLiteral("spanFn"));
    const QJsonArray defs = arr(r, "definitions");
    ASSERT_EQ(defs.size(), 1) << dump(r);
    const QJsonObject d = defs.at(0).toObject();
    EXPECT_EQ(d.value(QStringLiteral("file")).toString(), QStringLiteral("span.cpp"));
    EXPECT_EQ(d.value(QStringLiteral("line")).toInt(), 3);
    EXPECT_EQ(d.value(QStringLiteral("end_line")).toInt(), 9);
}

// INV-3 — a prototype and two overloads: three entries, one a declaration.
TEST_F(TraceServe, Inv3PrototypeAndOverloadsAreSeparateEntries) {
    const QJsonObject r = traceSymbol(QStringLiteral("overloaded"));
    const QJsonArray defs = arr(r, "definitions");
    const auto isDecl = [](const QJsonObject &d) {
        return d.value(QStringLiteral("is_declaration")).toBool();
    };
    const QJsonArray decls = rowsWhere(defs, isDecl);
    const QJsonArray bodies = rowsWhere(defs, [&](const QJsonObject &d) { return !isDecl(d); });
    ASSERT_EQ(decls.size(), 1) << dump(defs);
    EXPECT_EQ(decls.at(0).toObject().value(QStringLiteral("file")).toString(),
              QStringLiteral("overloads.h"));
    ASSERT_EQ(bodies.size(), 2) << dump(defs);
    for (const QJsonValue &b : bodies)
        EXPECT_EQ(b.toObject().value(QStringLiteral("file")).toString(),
                  QStringLiteral("overloads.cpp"));
}

// INV-4 — one row per call site, named by the innermost enclosing definition.
TEST_F(TraceServe, Inv4OneCallerRowPerCallSiteInnermostEnclosing) {
    const QJsonObject r = traceSymbol(QStringLiteral("callee"));
    const QJsonArray callers = arr(r, "callers");
    ASSERT_EQ(callers.size(), 3) << dump(r);
    QStringList names;
    for (const QJsonValue &c : callers)
        names << c.toObject().value(QStringLiteral("caller")).toObject()
                     .value(QStringLiteral("name")).toString();
    EXPECT_EQ(names, (QStringList{QStringLiteral("outer"), QStringLiteral("outer"),
                                  QStringLiteral("inner")}));
}

// INV-5 — each occurrence gets exactly one reference kind.
TEST_F(TraceServe, Inv5EachOccurrenceGetsOneReferenceKind) {
    const QJsonObject r = traceSymbol(QStringLiteral("kindProbe"));
    QMap<QString, int> kinds;
    for (const QJsonValue &v : arr(r, "references"))
        ++kinds[v.toObject().value(QStringLiteral("kind")).toString()];
    const QMap<QString, int> expected{{QStringLiteral("import"), 1},
                                      {QStringLiteral("code"), 1},
                                      {QStringLiteral("comment"), 1},
                                      {QStringLiteral("string_or_config"), 2},
                                      {QStringLiteral("test"), 1},
                                      {QStringLiteral("unparsed"), 1}};
    EXPECT_EQ(kinds, expected) << dump(arr(r, "references"));
}

// INV-6 — a document row carries its nearest heading; a fenced match is not one.
TEST_F(TraceServe, Inv6DocRowCarriesNearestHeading) {
    const QJsonObject r = traceSymbol(QStringLiteral("docProbe"));
    const QJsonArray docs = arr(r, "docs");
    ASSERT_EQ(docs.size(), 1) << dump(docs);
    const QJsonObject d = docs.at(0).toObject();
    EXPECT_EQ(d.value(QStringLiteral("file")).toString(), QStringLiteral("docs/guide.md"));
    EXPECT_EQ(d.value(QStringLiteral("heading")).toString(), QStringLiteral("Setup"));
    EXPECT_EQ(d.value(QStringLiteral("heading_line")).toInt(), 3);
    EXPECT_EQ(d.value(QStringLiteral("line")).toInt(), 5);
}

// INV-7 — a spec invariant and a roadmap bullet naming the symbol, by id.
TEST_F(TraceServe, Inv7SpecInvariantAndRoadmapItemById) {
    const QJsonObject r = traceSymbol(QStringLiteral("specProbe"));
    const QJsonArray specs = arr(r, "spec_invariants");
    ASSERT_EQ(specs.size(), 1) << dump(r);
    EXPECT_EQ(specs.at(0).toObject().value(QStringLiteral("spec")).toString(),
              QStringLiteral("FIX-0001"));
    EXPECT_EQ(specs.at(0).toObject().value(QStringLiteral("inv")).toString(),
              QStringLiteral("INV-2"));
    const QJsonArray items = arr(r, "roadmap_items");
    ASSERT_EQ(items.size(), 1) << dump(r);
    EXPECT_EQ(items.at(0).toObject().value(QStringLiteral("id")).toString(),
              QStringLiteral("FIX-0002"));
}

// INV-8 — a capped reply is never trimmed silently; the pages rebuild it.
TEST_F(TraceServe, Inv8PagingRebuildsTheWholeAnswer) {
    const QString sym = QStringLiteral("pagedProbe");
    const QJsonObject whole = call({{QStringLiteral("symbol"), sym},
                                    {QStringLiteral("max_bytes"), 16 * 1024 * 1024}});
    ASSERT_FALSE(whole.value(QStringLiteral("truncated")).toBool(true));
    const QStringList all = flatRows(whole);
    ASSERT_GE(all.size(), 300);

    // A cap that fits about 50 rows beside the envelope.
    qint64 rowBytes = 0;
    for (const QString &row : all) rowBytes += row.toUtf8().size();
    const qint64 envelope =
        QJsonDocument(whole).toJson(QJsonDocument::Compact).size() - rowBytes;
    const int maxBytes = int(envelope + rowBytes * 50 / all.size());

    QStringList joined;
    int offset = 0, pages = 0;
    for (;;) {
        ASSERT_LT(++pages, 100) << "paging does not end";
        QJsonObject args{{QStringLiteral("symbol"), sym},
                         {QStringLiteral("max_bytes"), maxBytes}};
        if (offset) args.insert(QStringLiteral("offset"), offset);
        const QJsonObject page = call(args);
        ASSERT_TRUE(page.value(QStringLiteral("ok")).toBool()) << dump(page);
        EXPECT_LE(QJsonDocument(page).toJson(QJsonDocument::Compact).size(), maxBytes)
            << "page " << pages;
        const QStringList rows = flatRows(page);
        ASSERT_FALSE(rows.isEmpty()) << "page " << pages << " is empty";
        joined << rows;
        const QJsonValue next = page.value(QStringLiteral("next_offset"));
        if (next.isNull()) {
            EXPECT_FALSE(page.value(QStringLiteral("truncated")).toBool(true));
            break;
        }
        EXPECT_TRUE(page.value(QStringLiteral("truncated")).toBool()) << "page " << pages;
        EXPECT_EQ(next.toInt(), offset + int(rows.size()));
        offset = next.toInt();
    }
    EXPECT_GE(pages, 4);
    EXPECT_EQ(joined, all);
}

// INV-9 — a file in a language with no grammar is reported, not dropped.
TEST_F(TraceServe, Inv9NoGrammarLanguageIsReported) {
    const QJsonObject r = traceSymbol(QStringLiteral("kindProbe"));
    const auto ng = notCovered(r, QStringLiteral("no_grammar"));
    ASSERT_TRUE(ng.has_value()) << dump(arr(r, "not_covered"));
    EXPECT_EQ(ng->value(QStringLiteral("lang")).toString(), QStringLiteral("zig"));
    EXPECT_EQ(ng->value(QStringLiteral("files")).toInt(), 1);
    EXPECT_EQ(rowsWhere(arr(r, "references"), [](const QJsonObject &o) {
                  return o.value(QStringLiteral("file")).toString() == QLatin1String("script.zig") &&
                         o.value(QStringLiteral("kind")).toString() == QLatin1String("unparsed");
              }).size(),
              1);
}

// INV-10 — an empty answer says what it looked at.
TEST_F(TraceServe, Inv10EmptyAnswerSaysWhatItScanned) {
    const QJsonObject r = traceSymbol(QStringLiteral("noSuchSymbol"));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool()) << dump(r);
    for (const char *s : {"definitions", "callers", "references", "docs", "spec_invariants",
                          "roadmap_items"})
        EXPECT_TRUE(arr(r, s).isEmpty()) << s;
    EXPECT_TRUE(r.value(QStringLiteral("tests")).toObject()
                    .value(QStringLiteral("files")).toArray().isEmpty());
    const QJsonObject scanned = r.value(QStringLiteral("scanned")).toObject();
    EXPECT_GT(scanned.value(QStringLiteral("files")).toInt(), 0);
    QSet<QString> langs;
    for (const QJsonValue &v : scanned.value(QStringLiteral("languages")).toArray())
        langs.insert(v.toString());
    const QSet<QString> fixtureLangs{
        QStringLiteral("c"),          QStringLiteral("cpp"),  QStringLiteral("python"),
        QStringLiteral("bash"),       QStringLiteral("lua"),  QStringLiteral("glsl"),
        QStringLiteral("javascript"), QStringLiteral("typescript"), QStringLiteral("tsx"),
        QStringLiteral("rust"),       QStringLiteral("go"),   QStringLiteral("java")};
    EXPECT_EQ(langs, fixtureLangs) << dump(scanned);
}

// INV-11 — path mode finds tests, documents and config naming a file; a
// path outside the root refuses; a non-unique basename is not searched.
TEST_F(TraceServe, Inv11PathModeFindsTestsAndDocs) {
    const QJsonObject args{{QStringLiteral("path"), QStringLiteral("units/probe.service")}};
    const QJsonObject r = call(args);
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool()) << dump(r);
    const QJsonArray tests = r.value(QStringLiteral("tests")).toObject()
                                 .value(QStringLiteral("files")).toArray();
    ASSERT_EQ(tests.size(), 1) << dump(r);
    EXPECT_EQ(tests.at(0).toString(), QStringLiteral("tests/test_units.py"));
    const QJsonArray docs = arr(r, "docs");
    ASSERT_EQ(docs.size(), 1) << dump(docs);
    EXPECT_EQ(docs.at(0).toObject().value(QStringLiteral("file")).toString(),
              QStringLiteral("docs/guide.md"));
    const auto inConfig = [](const QJsonObject &o) {
        return o.value(QStringLiteral("file")).toString() == QLatin1String("config.json");
    };
    // config.json names only the basename: found while the basename is unique.
    EXPECT_EQ(rowsWhere(arr(r, "references"), inConfig).size(), 1) << dump(r);
    EXPECT_FALSE(notCovered(r, QStringLiteral("ambiguous_basename")).has_value());

    const QJsonObject out = call({{QStringLiteral("path"), QStringLiteral("../../etc/passwd")}});
    EXPECT_FALSE(out.value(QStringLiteral("ok")).toBool(true));
    EXPECT_EQ(out.value(QStringLiteral("code")).toString(), QStringLiteral("bad_path")) << dump(out);

    ASSERT_TRUE(write(QStringLiteral("other/probe.service"), "[Unit]\n"));
    const QJsonObject amb = call(args);
    ASSERT_TRUE(notCovered(amb, QStringLiteral("ambiguous_basename")).has_value())
        << dump(arr(amb, "not_covered"));
    EXPECT_TRUE(rowsWhere(arr(amb, "references"), inConfig).isEmpty()) << dump(amb);
    EXPECT_EQ(amb.value(QStringLiteral("tests")).toObject()
                  .value(QStringLiteral("files")).toArray().size(), 1);
}

// INV-12 — the walk never follows a symlink.
TEST_F(TraceServe, Inv12WalkDoesNotFollowSymlinks) {
    QTemporaryDir outside;
    ASSERT_TRUE(outside.isValid());
    QFile leak(outside.path() + QStringLiteral("/leak.py"));
    ASSERT_TRUE(leak.open(QIODevice::WriteOnly));
    leak.write("kindProbe = 1\n");
    leak.close();
    ASSERT_TRUE(QFile::link(outside.path(), m_root + QStringLiteral("/linked")));

    const QJsonObject r = traceSymbol(QStringLiteral("kindProbe"));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool()) << dump(r);
    for (const QJsonValue &v : arr(r, "references"))
        EXPECT_FALSE(v.toObject().value(QStringLiteral("file")).toString()
                         .startsWith(QLatin1String("linked")))
            << dump(v);
}

// INV-14 — an edited query file changes the next answer without a restart.
TEST_F(TraceServe, Inv14EditedQueryFileChangesNextAnswer) {
    const QJsonObject before = traceSymbol(QStringLiteral("queryProbe"));
    ASSERT_EQ(arr(before, "callers").size(), 1) << dump(before);

    // python.scm without its @reference.call patterns.
    const QByteArray builtin = [] {
        QFile f(QStringLiteral(":/trace-queries/python.scm"));
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }();
    ASSERT_FALSE(builtin.isEmpty());
    QByteArray edited;
    for (const QByteArray &line : builtin.split('\n'))
        if (!line.contains("@reference.call")) edited += line + '\n';
    ASSERT_NE(edited, builtin + '\n');
    QDir().mkpath(m_data.path() + QStringLiteral("/ants-terminal/trace-queries"));
    QFile user(m_data.path() + QStringLiteral("/ants-terminal/trace-queries/python.scm"));
    ASSERT_TRUE(user.open(QIODevice::WriteOnly));
    user.write(edited);
    user.close();

    const QJsonObject after = traceSymbol(QStringLiteral("queryProbe"));
    ASSERT_TRUE(after.value(QStringLiteral("ok")).toBool()) << dump(after);
    EXPECT_TRUE(arr(after, "callers").isEmpty()) << dump(after);
    EXPECT_EQ(arr(after, "definitions").size(), 1);
}

// INV-15 — a file whose parse has errors still yields its definitions.
TEST_F(TraceServe, Inv15ParseErrorsStillYieldDefinitions) {
    const QJsonObject r = traceSymbol(QStringLiteral("afterError"));
    const QJsonArray defs = arr(r, "definitions");
    ASSERT_EQ(defs.size(), 1) << dump(r);
    EXPECT_EQ(defs.at(0).toObject().value(QStringLiteral("file")).toString(),
              QStringLiteral("broken.cpp"));
    EXPECT_GE(r.value(QStringLiteral("scanned")).toObject()
                  .value(QStringLiteral("files_with_parse_errors")).toInt(), 1)
        << dump(r.value(QStringLiteral("scanned")));
}

// INV-16 — a file over 2 MiB is reported, not parsed.
TEST_F(TraceServe, Inv16OversizeFileReportedNotParsed) {
    QByteArray big = "kindProbe = 1\n";
    const QByteArray filler(79, '#');
    while (big.size() < 3 * 1024 * 1024) big += filler + '\n';
    ASSERT_TRUE(write(QStringLiteral("big.py"), big));

    const QJsonObject r = traceSymbol(QStringLiteral("kindProbe"));
    const auto tl = notCovered(r, QStringLiteral("file_too_large"));
    ASSERT_TRUE(tl.has_value()) << dump(arr(r, "not_covered"));
    EXPECT_EQ(tl->value(QStringLiteral("file")).toString(), QStringLiteral("big.py"));
    EXPECT_TRUE(rowsWhere(arr(r, "references"), [](const QJsonObject &o) {
                    return o.value(QStringLiteral("file")).toString() == QLatin1String("big.py");
                }).isEmpty());
}

// INV-18 — ship 1 refuses depth above 1 and callees, rather than answering
// at depth 1.
TEST_F(TraceServe, Inv18ShipOneRefusesDepthAndCallees) {
    for (const QJsonObject &args :
         {QJsonObject{{QStringLiteral("symbol"), QStringLiteral("callee")},
                      {QStringLiteral("depth"), 2}},
          QJsonObject{{QStringLiteral("symbol"), QStringLiteral("callee")},
                      {QStringLiteral("include"), QJsonArray{QStringLiteral("callees")}}}}) {
        SCOPED_TRACE(dump(args));
        const QJsonObject r = call(args);
        EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool(true));
        EXPECT_EQ(r.value(QStringLiteral("code")).toString(), QStringLiteral("bad_args"));
    }
}
