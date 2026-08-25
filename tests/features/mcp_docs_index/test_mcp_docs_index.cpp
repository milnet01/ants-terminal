// Feature-conformance test for the docs_index MCP tool (ANTS-2139).
// Behavioural invariants drive the pure DocsIndex helper; wiring invariants
// source-scrape the registration sites. See spec.md + docs/specs/ANTS-2139.md.

#include "../../_support/expect.h"
#include "docsindex.h"
#include "mcpprojection.h"

#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#ifndef ANTS_SOURCE_DIR
#error "ANTS_SOURCE_DIR compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

using namespace DocsIndex;

bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}
std::string srcPath(const char *rel) {
    return std::string(ANTS_SOURCE_DIR) + "/" + rel;
}

void writeFile(const QString &path, const QString &content) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(content.toUtf8());
    f.close();
}

QueryParams summaryQ() { return QueryParams{}; }

const DocEntry *findEntry(const Index &idx, const QString &rel) {
    for (const DocEntry &de : idx.docs)
        if (de.path == rel) return &de;
    return nullptr;
}

}  // namespace

// INV-1 — cold build shape + doc-less root.
TEST(DocsIndex, BuildShapeAndEmptyRoot) {
    QTemporaryDir dir;
    writeFile(dir.path() + "/README.md",
              QStringLiteral("# Project Title\n## Usage\nhi\n"));
    writeFile(dir.path() + "/docs/specs/alpha.md",
              QStringLiteral("# Alpha Spec\n## Surface\n"));
    Index idx = build(dir.path(), 1000);
    ASSERT_EQ(idx.docs.size(), 2);
    // Sorted: root README.md before docs/specs/alpha.md.
    EXPECT_EQ(idx.docs[0].path, QStringLiteral("README.md"));
    EXPECT_EQ(idx.docs[0].id, QStringLiteral("README"));
    EXPECT_EQ(idx.docs[0].title, QStringLiteral("Project Title"));
    const DocEntry *a = findEntry(idx, QStringLiteral("docs/specs/alpha.md"));
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->id, QStringLiteral("alpha"));
    EXPECT_EQ(a->title, QStringLiteral("Alpha Spec"));
    bool sawSurface = false;
    for (const Heading &h : a->headings)
        if (h.text == QStringLiteral("Surface")) sawSurface = true;
    EXPECT_TRUE(sawSurface);

    QTemporaryDir empty;
    Index e = build(empty.path(), 1000);
    EXPECT_EQ(e.docs.size(), 0);
    QJsonObject env = query(e, summaryQ(), 0, QStringLiteral("/c"));
    EXPECT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(env.value("doc_count").toInt(), 0);
}

// INV-2 — refresh: changed re-scanned, removed pruned, added present;
// refreshedOut == changed+added; untouched reused.
TEST(DocsIndex, RefreshDeltaAddedRemoved) {
    QTemporaryDir dir;
    writeFile(dir.path() + "/docs/keep.md",  QStringLiteral("# Keep\n"));
    writeFile(dir.path() + "/docs/change.md", QStringLiteral("# Change v1\n"));
    writeFile(dir.path() + "/docs/gone.md",   QStringLiteral("# Gone\n"));
    Index prev = build(dir.path(), 1000);
    ASSERT_EQ(prev.docs.size(), 3);

    // Force change.md's stored mtime to differ from disk (deterministic
    // "changed" signal independent of filesystem mtime resolution).
    for (DocEntry &de : prev.docs)
        if (de.path == QStringLiteral("docs/change.md")) de.mtimeMs = 1;

    QFile::remove(dir.path() + "/docs/gone.md");
    writeFile(dir.path() + "/docs/added.md", QStringLiteral("# Added\n"));

    int refreshed = -1;
    Index cur = refresh(prev, dir.path(), 2000, Options{}, &refreshed);
    EXPECT_EQ(refreshed, 2);  // change + added
    QStringList paths;
    for (const DocEntry &de : cur.docs) paths << de.path;
    EXPECT_TRUE(paths.contains(QStringLiteral("docs/keep.md")));
    EXPECT_TRUE(paths.contains(QStringLiteral("docs/added.md")));
    EXPECT_FALSE(paths.contains(QStringLiteral("docs/gone.md")));
    // keep.md was untouched → reused (mtime != the forced sentinel).
    const DocEntry *keep = findEntry(cur, QStringLiteral("docs/keep.md"));
    ASSERT_NE(keep, nullptr);
    EXPECT_EQ(keep->title, QStringLiteral("Keep"));
}

// INV-3 — ATX levels, 1-based line, title from first H1, no-H1 → "",
// over-long line skipped.
TEST(DocsIndex, HeadingLevelsTitleLongLine) {
    QTemporaryDir dir;
    writeFile(dir.path() + "/docs/levels.md",
              QStringLiteral("# Title One\n## Sub Two\n### Deep Three\n"));
    // A doc whose first heading is level 2 → no title.
    writeFile(dir.path() + "/docs/noh1.md",
              QStringLiteral("## Only A Sub\nbody\n"));
    // An over-long heading line (> kMaxLineBytes) must be dropped.
    const QString longHeading =
        QStringLiteral("# ") + QString(kMaxLineBytes + 50, QLatin1Char('x'));
    writeFile(dir.path() + "/docs/longline.md",
              QStringLiteral("# Good Head\n") + longHeading + QStringLiteral("\n"));
    Index idx = build(dir.path(), 1000);

    const DocEntry *lv = findEntry(idx, QStringLiteral("docs/levels.md"));
    ASSERT_NE(lv, nullptr);
    ASSERT_EQ(lv->headings.size(), 3);
    EXPECT_EQ(lv->headings[0].level, 1);
    EXPECT_EQ(lv->headings[0].line, 1);
    EXPECT_EQ(lv->headings[1].level, 2);
    EXPECT_EQ(lv->headings[2].level, 3);
    EXPECT_EQ(lv->title, QStringLiteral("Title One"));

    const DocEntry *nh = findEntry(idx, QStringLiteral("docs/noh1.md"));
    ASSERT_NE(nh, nullptr);
    EXPECT_EQ(nh->title, QString());

    const DocEntry *ll = findEntry(idx, QStringLiteral("docs/longline.md"));
    ASSERT_NE(ll, nullptr);
    EXPECT_EQ(ll->headings.size(), 1);  // over-long heading skipped
    EXPECT_EQ(ll->headings[0].text, QStringLiteral("Good Head"));
}

// INV-4 — link extraction: a doc at docs/specs/d.md.
TEST(DocsIndex, LinkExtraction) {
    QTemporaryDir dir;
    writeFile(dir.path() + "/docs/specs/d.md",
              QStringLiteral("# D\n"
                             "[a](sib.md) [b](../x/y.md#sec) "
                             "[c](https://e.com) [d](img.png) "
                             "[e](sib.md)\n"));  // dup of (a)
    Index idx = build(dir.path(), 1000);
    const DocEntry *d = findEntry(idx, QStringLiteral("docs/specs/d.md"));
    ASSERT_NE(d, nullptr);
    QStringList expect{QStringLiteral("docs/specs/sib.md"),
                       QStringLiteral("docs/x/y.md")};
    EXPECT_EQ(d->links, expect);  // sorted, deduped, anchor stripped
    // docs/x/y.md is kept even though no such doc is indexed.
    EXPECT_EQ(findEntry(idx, QStringLiteral("docs/x/y.md")), nullptr);
}

// ANTS-3604 — headings and links inside a fenced code block are illustrative,
// not real index content, and must be skipped (fence-aware scan).
TEST(DocsIndex, FenceAwareScan) {
    QTemporaryDir dir;
    writeFile(dir.path() + "/docs/fence.md",
              QStringLiteral("# Real Title\n"
                             "[real](sib.md)\n"
                             "\n"
                             "```\n"
                             "# Fake Heading\n"
                             "[fake](ghost.md)\n"
                             "```\n"
                             "## Real Sub\n"));
    Index idx = build(dir.path(), 1000);
    const DocEntry *d = findEntry(idx, QStringLiteral("docs/fence.md"));
    ASSERT_NE(d, nullptr);
    // Only the two real headings; the fenced "# Fake Heading" is excluded.
    ASSERT_EQ(d->headings.size(), 2);
    EXPECT_EQ(d->headings[0].text, QStringLiteral("Real Title"));
    EXPECT_EQ(d->headings[1].text, QStringLiteral("Real Sub"));
    // Only the real link; the fenced [fake](ghost.md) is excluded.
    EXPECT_EQ(d->links, (QStringList{QStringLiteral("docs/sib.md")}));
}

// INV-5 — selector arity.
TEST(DocsIndex, SelectorArity) {
    QTemporaryDir dir;
    writeFile(dir.path() + "/docs/a.md", QStringLiteral("# A\n"));
    Index idx = build(dir.path(), 1000);

    QueryParams two; two.topic = "x"; two.id = "y";
    QJsonObject bad = query(idx, two, 0, QStringLiteral("/c"));
    EXPECT_FALSE(bad.value("ok").toBool());
    EXPECT_EQ(bad.value("code").toString(), QStringLiteral("bad_args"));

    QueryParams emptySel; emptySel.topic = "";  // empty == absent → summary
    QJsonObject es = query(idx, emptySel, 0, QStringLiteral("/c"));
    EXPECT_TRUE(es.value("ok").toBool());
    EXPECT_TRUE(es.contains("docs"));

    QJsonObject sum = query(idx, summaryQ(), 0, QStringLiteral("/c"));
    EXPECT_TRUE(sum.value("ok").toBool());
    EXPECT_TRUE(sum.contains("docs"));
    EXPECT_TRUE(sum.contains("dirs"));
    EXPECT_FALSE(sum.contains("matches"));  // never a full dump
}

// INV-6 — topic scoring, cap, miss.
TEST(DocsIndex, TopicScoreCapMiss) {
    QTemporaryDir dir;
    // alpha: path "spec" (+2) + heading "Spec details" (+1) = 3.
    writeFile(dir.path() + "/docs/specs/alpha.md",
              QStringLiteral("# Alpha Title\n## Spec details\n"));
    // beta: path "spec" (+2) only = 2.
    writeFile(dir.path() + "/docs/specs/beta.md",
              QStringLiteral("# Beta Title\n## Notes\n"));
    Index idx = build(dir.path(), 1000);

    QueryParams q; q.topic = "spec";
    Options o; o.maxTopicHits = 1;
    QJsonObject env = query(idx, q, 0, QStringLiteral("/c"), o);
    EXPECT_TRUE(env.value("found").toBool());
    EXPECT_TRUE(env.value("topic_truncated").toBool());
    const QJsonArray m = env.value("matches").toArray();
    ASSERT_EQ(m.size(), 1);
    EXPECT_EQ(m[0].toObject().value("path").toString(),
              QStringLiteral("docs/specs/alpha.md"));  // higher score survives
    EXPECT_GE(m[0].toObject().value("evidence").toArray().size(), 1);

    QueryParams miss; miss.topic = "zzzznope";
    QJsonObject mo = query(idx, miss, 0, QStringLiteral("/c"));
    EXPECT_TRUE(mo.value("ok").toBool());
    EXPECT_FALSE(mo.value("found").toBool());
    EXPECT_EQ(mo.value("matches").toArray().size(), 0);
}

// INV-7 — doc_path found / miss / linked_from.
TEST(DocsIndex, DocPathFoundMissLinkedFrom) {
    QTemporaryDir dir;
    writeFile(dir.path() + "/docs/target.md", QStringLiteral("# Target\n"));
    writeFile(dir.path() + "/docs/linker.md",
              QStringLiteral("# Linker\n[t](target.md)\n"));
    Index idx = build(dir.path(), 1000);

    QueryParams hit; hit.docPath = "docs/target.md";
    QJsonObject h = query(idx, hit, 0, QStringLiteral("/c"));
    EXPECT_TRUE(h.value("found").toBool());
    const QJsonObject entry = h.value("entry").toObject();
    EXPECT_EQ(entry.value("id").toString(), QStringLiteral("target"));
    const QJsonArray lf = entry.value("linked_from").toArray();
    ASSERT_EQ(lf.size(), 1);
    EXPECT_EQ(lf[0].toString(), QStringLiteral("docs/linker.md"));

    QueryParams miss; miss.docPath = "docs/missing.md";
    QJsonObject m = query(idx, miss, 0, QStringLiteral("/c"));
    EXPECT_TRUE(m.value("ok").toBool());
    EXPECT_FALSE(m.value("found").toBool());
}

// INV-8 — id= returns every stem match, case-sensitive.
TEST(DocsIndex, IdAllStemMatchesCaseSensitive) {
    QTemporaryDir dir;
    writeFile(dir.path() + "/docs/specs/ANTS-9.md",   QStringLiteral("# Spec 9\n"));
    writeFile(dir.path() + "/docs/journal/ANTS-9.md", QStringLiteral("# Journal 9\n"));
    Index idx = build(dir.path(), 1000);

    QueryParams q; q.id = "ANTS-9";
    QJsonObject env = query(idx, q, 0, QStringLiteral("/c"));
    EXPECT_TRUE(env.value("found").toBool());
    const QJsonArray m = env.value("matches").toArray();
    ASSERT_EQ(m.size(), 2);  // both dirs, sorted by path
    EXPECT_EQ(m[0].toObject().value("path").toString(),
              QStringLiteral("docs/journal/ANTS-9.md"));
    EXPECT_FALSE(m[0].toObject().contains("id"));  // id omitted (== query)

    QueryParams wrong; wrong.id = "ants-9";  // wrong case
    QJsonObject w = query(idx, wrong, 0, QStringLiteral("/c"));
    EXPECT_TRUE(w.value("ok").toBool());
    EXPECT_FALSE(w.value("found").toBool());
}

// INV-9/10/11/13 — wiring source-scrapes.
TEST(DocsIndex, WiringRegistered) {
    const std::string ci = ants_test::slurpFile(srcPath("src/claudeintegration.cpp"));
    const std::string mw = ants_test::slurpFile(srcPath("src/mainwindow.cpp"));
    const std::string mp = ants_test::slurpFile(srcPath("src/mcpprojection.cpp"));
    const std::string rc = ants_test::slurpRemoteControl();
    const std::string di = ants_test::slurpFile(srcPath("src/docsindex.cpp"));

    // caller_cwd Required (INV-9).
    EXPECT_TRUE(has(mw, "registerToolProvider(\"docs_index\""));
    EXPECT_TRUE(has(ci, "docs_index"));            // contract + etag + schema
    // ETag-304 (INV-10) — registered, handler emits no etag.
    EXPECT_TRUE(has(ci, "isEtagSupportedTool"));
    EXPECT_FALSE(has(di, "\"etag\""));
    // Compaction table (INV-11). `fields=` needs no enrolment since
    // ANTS-4524 — every verb honours it; this row is the compaction answer.
    EXPECT_TRUE(has(mp, "docs_index"));
    EXPECT_TRUE(has(mp, "isDefaultCompactTool"));
    // handler routes doc_path through PathValidation (INV-9 path defence).
    EXPECT_TRUE(has(rc, "cmdDocsIndex"));
    EXPECT_TRUE(has(rc, "validatePath"));
    // atomic cache write (INV-13).
    EXPECT_TRUE(has(di, "QSaveFile"));
}

// INV-10 (behavioural) — warm serve stability.
TEST(DocsIndex, WarmServeStable) {
    QTemporaryDir dir;
    writeFile(dir.path() + "/docs/a.md", QStringLiteral("# A\n## S\n"));
    const QString cache = dir.path() + "/cache.json";

    QJsonObject c1 = serve(dir.path(), 1000, summaryQ(), Options{}, cache);
    QJsonObject c2 = serve(dir.path(), 2000, summaryQ(), Options{}, cache);
    QJsonObject c3 = serve(dir.path(), 3000, summaryQ(), Options{}, cache);
    EXPECT_GE(c1.value("refreshed_docs").toInt(), 1);  // cold build
    EXPECT_EQ(c2.value("refreshed_docs").toInt(), 0);  // warm
    EXPECT_EQ(c3.value("refreshed_docs").toInt(), 0);  // warm
    // generated_at_ms byte-identical across the two warm calls (etag stable).
    EXPECT_EQ(c2.value("generated_at_ms").toDouble(),
              c3.value("generated_at_ms").toDouble());
    EXPECT_EQ(QJsonDocument(c2).toJson(QJsonDocument::Compact),
              QJsonDocument(c3).toJson(QJsonDocument::Compact));
}

// INV-11 (behavioural) — fields projection narrows a summary body.
TEST(DocsIndex, FieldsProjectionNarrows) {
    QTemporaryDir dir;
    writeFile(dir.path() + "/docs/a.md", QStringLiteral("# A\n"));
    Index idx = build(dir.path(), 1000);
    const QJsonObject env = query(idx, summaryQ(), 0, QStringLiteral("/c"));
    const QString body = QString::fromUtf8(
        QJsonDocument(env).toJson(QJsonDocument::Compact));
    QJsonArray fields; fields.append(QStringLiteral("docs"));
    const QString narrowed = mcp::projectFields(body, fields);
    const QJsonObject n =
        QJsonDocument::fromJson(narrowed.toUtf8()).object();
    EXPECT_TRUE(n.contains("docs"));
    EXPECT_FALSE(n.contains("dirs"));
    EXPECT_FALSE(n.contains("doc_count"));
}

// INV-12 — doc-count ceiling: root *.md before docs/.
TEST(DocsIndex, DocCountCeiling) {
    QTemporaryDir dir;
    writeFile(dir.path() + "/r1.md", QStringLiteral("# R1\n"));
    writeFile(dir.path() + "/r2.md", QStringLiteral("# R2\n"));
    writeFile(dir.path() + "/docs/d1.md", QStringLiteral("# D1\n"));
    Options o; o.maxIndexDocs = 2;
    Index idx = build(dir.path(), 1000, o);
    EXPECT_TRUE(idx.docsTruncated);
    ASSERT_EQ(idx.docs.size(), 2);
    EXPECT_EQ(idx.docs[0].path, QStringLiteral("r1.md"));
    EXPECT_EQ(idx.docs[1].path, QStringLiteral("r2.md"));  // docs/d1.md dropped
}

// INV-13 — round-trip serialisation equality.
TEST(DocsIndex, SerialisationRoundTrip) {
    QTemporaryDir dir;
    writeFile(dir.path() + "/docs/a.md",
              QStringLiteral("# A\n## S\n[l](b.md)\n"));
    writeFile(dir.path() + "/docs/b.md", QStringLiteral("# B\n"));
    Index idx = build(dir.path(), 1000);
    const QByteArray a = QJsonDocument(toJson(idx)).toJson(QJsonDocument::Compact);
    Index rt = fromJson(toJson(idx));
    const QByteArray b = QJsonDocument(toJson(rt)).toJson(QJsonDocument::Compact);
    EXPECT_EQ(a, b);
    EXPECT_EQ(rt.docs.size(), idx.docs.size());
}

// INV-14 — version-0 / foreign-root / garbage cache rebuilds.
TEST(DocsIndex, StaleCacheRebuilds) {
    QTemporaryDir dir;
    writeFile(dir.path() + "/docs/a.md", QStringLiteral("# A\n"));

    auto serveWith = [&](const QString &seed) {
        const QString cache = dir.path() + "/c.json";
        writeFile(cache, seed);
        QJsonObject env = serve(dir.path(), 1000, summaryQ(), Options{}, cache);
        QFile f(cache);
        EXPECT_TRUE(f.open(QIODevice::ReadOnly));
        Index loaded = fromJson(QJsonDocument::fromJson(f.readAll()).object());
        EXPECT_EQ(loaded.version, kIndexVersion);
        EXPECT_EQ(loaded.rootCanonical, dir.path());
        EXPECT_EQ(env.value("doc_count").toInt(), 1);
    };
    serveWith(QStringLiteral("{\"version\":0,\"root_canonical\":\"%1\"}").arg(dir.path()));
    serveWith(QStringLiteral("{\"version\":1,\"root_canonical\":\"/somewhere/else\"}"));
    serveWith(QStringLiteral("}{ not json"));
}

// INV-15 — byte ceiling: tiny maxCacheBytes truncates, prefix survives.
TEST(DocsIndex, ByteCeiling) {
    QTemporaryDir dir;
    writeFile(dir.path() + "/docs/a.md", QStringLiteral("# A\n"));
    writeFile(dir.path() + "/docs/b.md", QStringLiteral("# B\n"));
    writeFile(dir.path() + "/docs/c.md", QStringLiteral("# C\n"));
    Options o; o.maxCacheBytes = 1;  // forces stop after the first doc
    Index idx = build(dir.path(), 1000, o);
    EXPECT_TRUE(idx.docsTruncated);
    ASSERT_EQ(idx.docs.size(), 1);
    EXPECT_EQ(idx.docs[0].path, QStringLiteral("docs/a.md"));
}

// INV-16 — summary count integrity.
TEST(DocsIndex, SummaryCountIntegrity) {
    QTemporaryDir dir;
    writeFile(dir.path() + "/README.md", QStringLiteral("# R\n## S1\n## S2\n"));
    writeFile(dir.path() + "/docs/specs/a.md", QStringLiteral("# A\n"));
    writeFile(dir.path() + "/docs/specs/b.md", QStringLiteral("# B\n"));
    writeFile(dir.path() + "/docs/standards/c.md", QStringLiteral("# C\n"));
    Index idx = build(dir.path(), 1000);
    QJsonObject env = query(idx, summaryQ(), 0, QStringLiteral("/c"));
    const int dc = env.value("doc_count").toInt();
    EXPECT_EQ(dc, 4);
    const QJsonArray docs = env.value("docs").toArray();
    EXPECT_EQ(docs.size(), dc);
    int dirSum = 0;
    for (const QJsonValue &v : env.value("dirs").toArray())
        dirSum += v.toObject().value("doc_count").toInt();
    EXPECT_EQ(dirSum, dc);
    // README has 3 headings (# R, ## S1, ## S2) → heading_count == length.
    for (const QJsonValue &v : docs) {
        const QJsonObject d = v.toObject();
        if (d.value("path").toString() == QStringLiteral("README.md")) {
            EXPECT_EQ(d.value("heading_count").toInt(), 3);
        }
    }
}

// INV-17 — status best-effort.
TEST(DocsIndex, StatusBestEffort) {
    QTemporaryDir dir;
    writeFile(dir.path() + "/docs/withstatus.md",
              QStringLiteral("# Spec\n**Status:** accepted (2026-06-15)\n"));
    writeFile(dir.path() + "/docs/nostatus.md", QStringLiteral("# Plain\n"));
    Index idx = build(dir.path(), 1000);
    const DocEntry *ws = findEntry(idx, QStringLiteral("docs/withstatus.md"));
    const DocEntry *ns = findEntry(idx, QStringLiteral("docs/nostatus.md"));
    ASSERT_NE(ws, nullptr);
    ASSERT_NE(ns, nullptr);
    EXPECT_EQ(ws->status, QStringLiteral("accepted (2026-06-15)"));
    EXPECT_EQ(ns->status, QString());  // absent, still indexed
}

// INV-18 — linked_from cap.
TEST(DocsIndex, LinkedFromCap) {
    QTemporaryDir dir;
    writeFile(dir.path() + "/docs/specs/target.md", QStringLiteral("# T\n"));
    writeFile(dir.path() + "/docs/specs/l1.md",
              QStringLiteral("# L1\n[t](target.md)\n"));
    writeFile(dir.path() + "/docs/specs/l2.md",
              QStringLiteral("# L2\n[t](target.md)\n"));
    Index idx = build(dir.path(), 1000);

    QueryParams q; q.docPath = "docs/specs/target.md";
    Options o; o.maxLinkedFrom = 1;
    QJsonObject env = query(idx, q, 0, QStringLiteral("/c"), o);
    const QJsonObject entry = env.value("entry").toObject();
    const QJsonArray lf = entry.value("linked_from").toArray();
    ASSERT_EQ(lf.size(), 1);
    EXPECT_TRUE(env.value("linked_from_truncated").toBool());
    EXPECT_EQ(lf[0].toString(), QStringLiteral("docs/specs/l1.md"));  // path-first
}

// INV-19 — silent per-doc caps (headings + doc-byte budget).
TEST(DocsIndex, PerDocCaps) {
    QTemporaryDir dir;
    writeFile(dir.path() + "/docs/many.md",
              QStringLiteral("# One\n## Two\n### Three\n"));
    Options ho; ho.maxHeadingsPerDoc = 1;
    Index hidx = build(dir.path(), 1000, ho);
    const DocEntry *many = findEntry(hidx, QStringLiteral("docs/many.md"));
    ASSERT_NE(many, nullptr);
    EXPECT_EQ(many->headings.size(), 1);  // first kept, rest dropped

    // doc-byte budget: only the first heading is read before the cutoff.
    QTemporaryDir d2;
    writeFile(d2.path() + "/docs/big.md",
              QStringLiteral("# First\n") + QString(200, QLatin1Char('y'))
              + QStringLiteral("\n# Second\n"));
    Options bo; bo.maxDocBytes = 8;  // "# First\n" == 8 bytes; stop before line 2
    Index bidx = build(d2.path(), 1000, bo);
    const DocEntry *big = findEntry(bidx, QStringLiteral("docs/big.md"));
    ASSERT_NE(big, nullptr);
    ASSERT_EQ(big->headings.size(), 1);  // entry emitted, only pre-budget heading
    EXPECT_EQ(big->headings[0].text, QStringLiteral("First"));
}
