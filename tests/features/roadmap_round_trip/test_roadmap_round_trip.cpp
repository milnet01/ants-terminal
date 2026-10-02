// Feature-conformance test for ANTS-3810 — the round-trip oracle (INV-1,
// INV-3) and whole-store relationship acyclicity (INV-2, INV-4).
// Contract: tests/features/roadmap_round_trip/spec.md
// Parent spec: docs/specs/ANTS-3810-round-trip-oracle-and-acyclicity.md
//
// Not roadmap_export_roundtrip/: that directory is export → rebuild →
// re-export (ANTS-3761's INV-1). This one is render → load → export
// (ANTS-3758's INV-1).

#include <gtest/gtest.h>

#include "jsoncanonical.h"
#include "roadmapcheck.h"
#include "roadmapexport.h"
#include "roadmapindex.h"
#include "roadmapmigrate.h"
#include "roadmapmigrateload.h"
#include "roadmapparse.h"
#include "roadmaprender.h"
#include "roadmapstore.h"

#include <QBuffer>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <memory>

namespace {

// ===================================================================== store

// NEVER default-construct a RoadmapStore: it resolves defaultPath(), the
// developer's REAL store.
std::unique_ptr<RoadmapStore> openStore(const QTemporaryDir &dir, const QString &name,
                                        RoadmapStore::Access access) {
    auto s = std::make_unique<RoadmapStore>(dir.filePath(name),
                                            RoadmapStore::kDefaultHistoryCapBytes, access);
    QString err;
    if (!s->open(&err)) {
        ADD_FAILURE() << "store open: " << err.toStdString();
        return nullptr;
    }
    return s;
}

RoadmapStore::ItemWrite mkItem(qint64 projectId, const QString &id, qint64 sectionId,
                               int position) {
    RoadmapStore::ItemWrite w;
    w.projectId = projectId;
    w.id = id;
    w.status = QStringLiteral("planned");
    w.headline = QStringLiteral("Item ") + id + QLatin1Char('.');
    w.kind = QStringLiteral("implement");
    w.source = QStringLiteral("test");
    w.layman = QStringLiteral("A plain sentence.");
    w.sectionId = sectionId;
    w.position = position;
    return w;
}

// ============================================================ INV-2 / INV-4

// One project with N items, ids "<P>-1".."<P>-N"; returns their pks.
struct Proj {
    qint64 id = 0;
    QVector<qint64> items;
};

Proj project(RoadmapStore &s, const QTemporaryDir &dir, const QString &slug, int n) {
    QString err;
    Proj p;
    QDir().mkpath(dir.filePath(slug));   // registerProject() canonicalises the root
    const auto pid = s.registerProject(dir.filePath(slug), slug, slug, &err);
    if (!pid) {
        ADD_FAILURE() << "registerProject: " << err.toStdString();
        return p;
    }
    p.id = *pid;
    const auto sec = s.addSection(p.id, QStringLiteral("s"), QStringLiteral("S"), 2, 1,
                                  std::nullopt, &err);
    if (!sec) {
        ADD_FAILURE() << "addSection: " << err.toStdString();
        return p;
    }
    for (int i = 1; i <= n; ++i) {
        const auto pk = s.putItem(
            mkItem(p.id, slug.toUpper() + QLatin1Char('-') + QString::number(i), *sec, i - 1),
            &err);
        if (!pk) {
            ADD_FAILURE() << "putItem: " << err.toStdString();
            return p;
        }
        p.items.append(*pk);
    }
    return p;
}

using Path = QVector<QPair<QString, QString>>;

Path pathOf(std::initializer_list<QPair<QString, QString>> keys) { return Path(keys); }

}  // namespace

// INV-2 — a cycle is reported, not refused and not ignored.
TEST(RoadmapRoundTrip, Inv2Acyclicity) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto s = openStore(dir, QStringLiteral("store.sqlite"), RoadmapStore::Access::Bulk);
    ASSERT_TRUE(s);
    const Proj p = project(*s, dir, QStringLiteral("aa"), 2);
    ASSERT_EQ(p.items.size(), 2);
    QString err;

    // (b) before the closing edge: no cycle, so the report is a function of
    // the graph and not of having been called.
    ASSERT_TRUE(s->relateItems(QStringLiteral("blocked-by"), p.items[0], p.items[1], &err))
        << err.toStdString();
    const auto open = RoadmapCheck::findRelationshipCycles(*s, &err);
    ASSERT_TRUE(open) << err.toStdString();
    EXPECT_TRUE(open->cycles.isEmpty());

    // (a) the closing write SUCCEEDS — the check reports, the store does not
    // refuse — and the cycle comes back once, in canonical rotation.
    ASSERT_TRUE(s->relateItems(QStringLiteral("blocked-by"), p.items[1], p.items[0], &err))
        << "relateItems refused a cycle-closing write: " << err.toStdString();
    const auto closed = RoadmapCheck::findRelationshipCycles(*s, &err);
    ASSERT_TRUE(closed) << err.toStdString();
    ASSERT_EQ(closed->cycles.size(), 1);
    EXPECT_EQ(closed->cycles.first().type, QStringLiteral("blocked-by"));
    EXPECT_EQ(closed->cycles.first().path,
              pathOf({{QStringLiteral("aa"), QStringLiteral("aa-1")},
                      {QStringLiteral("aa"), QStringLiteral("aa-2")}}));
    EXPECT_FALSE(closed->truncated);

    // (c) a FAILED check is nullopt with an error, never an empty report.
    RoadmapStore unopened(dir.filePath(QStringLiteral("never-opened.sqlite")));
    QString uerr;
    const auto failed = RoadmapCheck::findRelationshipCycles(unopened, &uerr);
    EXPECT_FALSE(failed) << "an unopened store reported a clean graph";
    EXPECT_FALSE(uerr.isEmpty());
}

// INV-4 — whole store, one type at a time, four acyclic types, every
// cross-project edge walked or counted, and a cap that says so.
TEST(RoadmapRoundTrip, Inv4CheckDomain) {
    QString err;

    // A cross-project cycle over three projects: aa-1 → bb-1 → cc-1 → aa-1.
    {
        QTemporaryDir dir;
        ASSERT_TRUE(dir.isValid());
        auto s = openStore(dir, QStringLiteral("store.sqlite"), RoadmapStore::Access::Bulk);
        ASSERT_TRUE(s);
        const Proj a = project(*s, dir, QStringLiteral("aa"), 1);
        const Proj b = project(*s, dir, QStringLiteral("bb"), 1);
        const Proj c = project(*s, dir, QStringLiteral("cc"), 1);
        ASSERT_TRUE(s->relateCrossProject(QStringLiteral("blocked-by"), a.items[0],
                                          QStringLiteral("bb"), QStringLiteral("bb-1"), &err));
        ASSERT_TRUE(s->relateCrossProject(QStringLiteral("blocked-by"), b.items[0],
                                          QStringLiteral("cc"), QStringLiteral("cc-1"), &err));
        ASSERT_TRUE(s->relateCrossProject(QStringLiteral("blocked-by"), c.items[0],
                                          QStringLiteral("aa"), QStringLiteral("aa-1"), &err));
        const auto r = RoadmapCheck::findRelationshipCycles(*s, &err);
        ASSERT_TRUE(r) << err.toStdString();
        ASSERT_EQ(r->cycles.size(), 1) << "a per-project walk cannot see this cycle";
        EXPECT_EQ(r->cycles.first().path,
                  pathOf({{QStringLiteral("aa"), QStringLiteral("aa-1")},
                          {QStringLiteral("bb"), QStringLiteral("bb-1")},
                          {QStringLiteral("cc"), QStringLiteral("cc-1")}}));
        EXPECT_EQ(r->unresolvedEdges, 0);
    }

    // Opposing directions under two types close no cycle: types are not folded.
    // And a relates-to triangle is not walked at all.
    {
        QTemporaryDir dir;
        ASSERT_TRUE(dir.isValid());
        auto s = openStore(dir, QStringLiteral("store.sqlite"), RoadmapStore::Access::Bulk);
        ASSERT_TRUE(s);
        const Proj p = project(*s, dir, QStringLiteral("aa"), 3);
        ASSERT_TRUE(s->relateItems(QStringLiteral("blocked-by"), p.items[0], p.items[1], &err));
        ASSERT_TRUE(s->relateItems(QStringLiteral("duplicate-of"), p.items[1], p.items[0], &err));
        ASSERT_TRUE(s->relateItems(QStringLiteral("relates-to"), p.items[0], p.items[1], &err));
        ASSERT_TRUE(s->relateItems(QStringLiteral("relates-to"), p.items[1], p.items[2], &err));
        ASSERT_TRUE(s->relateItems(QStringLiteral("relates-to"), p.items[2], p.items[0], &err));
        const auto r = RoadmapCheck::findRelationshipCycles(*s, &err);
        ASSERT_TRUE(r) << err.toStdString();
        EXPECT_TRUE(r->cycles.isEmpty())
            << "a folded graph, or a walked relates-to, reported a cycle";
    }

    // Both unresolved shapes are counted, not skipped: far project absent, and
    // far project present with no item at that id_fold.
    {
        QTemporaryDir dir;
        ASSERT_TRUE(dir.isValid());
        auto s = openStore(dir, QStringLiteral("store.sqlite"), RoadmapStore::Access::Bulk);
        ASSERT_TRUE(s);
        const Proj a = project(*s, dir, QStringLiteral("aa"), 1);
        ASSERT_TRUE(s->relateCrossProject(QStringLiteral("blocked-by"), a.items[0],
                                          QStringLiteral("zz"), QStringLiteral("zz-1"), &err));
        const auto absent = RoadmapCheck::findRelationshipCycles(*s, &err);
        ASSERT_TRUE(absent) << err.toStdString();
        EXPECT_TRUE(absent->cycles.isEmpty());
        EXPECT_EQ(absent->unresolvedEdges, 1) << "far project absent";
    }
    {
        QTemporaryDir dir;
        ASSERT_TRUE(dir.isValid());
        auto s = openStore(dir, QStringLiteral("store.sqlite"), RoadmapStore::Access::Bulk);
        ASSERT_TRUE(s);
        const Proj a = project(*s, dir, QStringLiteral("aa"), 1);
        project(*s, dir, QStringLiteral("bb"), 1);
        ASSERT_TRUE(s->relateCrossProject(QStringLiteral("blocked-by"), a.items[0],
                                          QStringLiteral("bb"), QStringLiteral("bb-9"), &err));
        const auto noItem = RoadmapCheck::findRelationshipCycles(*s, &err);
        ASSERT_TRUE(noItem) << err.toStdString();
        EXPECT_TRUE(noItem->cycles.isEmpty());
        EXPECT_EQ(noItem->unresolvedEdges, 1) << "far project present, no such item";
    }

    // Two back edges, two cycles — one per back edge, not one per SCC:
    // 1 → 2 → 1 and 2 → 3 → 2 share node 2 and form one component.
    {
        QTemporaryDir dir;
        ASSERT_TRUE(dir.isValid());
        auto s = openStore(dir, QStringLiteral("store.sqlite"), RoadmapStore::Access::Bulk);
        ASSERT_TRUE(s);
        const Proj p = project(*s, dir, QStringLiteral("aa"), 3);
        for (const auto &e : {qMakePair(0, 1), qMakePair(1, 0), qMakePair(1, 2), qMakePair(2, 1)})
            ASSERT_TRUE(s->relateItems(QStringLiteral("supersedes"), p.items[e.first],
                                       p.items[e.second], &err));
        const auto r = RoadmapCheck::findRelationshipCycles(*s, &err);
        ASSERT_TRUE(r) << err.toStdString();
        EXPECT_EQ(r->cycles.size(), 2);
    }

    // More back edges than the cap, one type only: the cap holds and says so.
    // A hub with kMaxCyclesPerType + 6 two-cycles hub ↔ leaf.
    {
        QTemporaryDir dir;
        ASSERT_TRUE(dir.isValid());
        auto s = openStore(dir, QStringLiteral("store.sqlite"), RoadmapStore::Access::Bulk);
        ASSERT_TRUE(s);
        const int leaves = RoadmapCheck::kMaxCyclesPerType + 6;
        const Proj p = project(*s, dir, QStringLiteral("aa"), leaves + 1);
        ASSERT_TRUE(s->begin(&err)) << err.toStdString();
        for (int i = 1; i <= leaves; ++i) {
            ASSERT_TRUE(s->relateItems(QStringLiteral("splits-from"), p.items[0], p.items[i], &err));
            ASSERT_TRUE(s->relateItems(QStringLiteral("splits-from"), p.items[i], p.items[0], &err));
        }
        ASSERT_TRUE(s->commit(&err)) << err.toStdString();
        const auto r = RoadmapCheck::findRelationshipCycles(*s, &err);
        ASSERT_TRUE(r) << err.toStdString();
        EXPECT_EQ(r->cycles.size(), RoadmapCheck::kMaxCyclesPerType);
        EXPECT_TRUE(r->truncated);
    }
}

// ============================================================ INV-1 / INV-3

namespace {

// A file head as a migrated store holds it: the format marker, the render's
// generated-file notice (which a re-load keeps), then the H1.
QString headIntro(const QString &h1 = QString()) {
    QString t = QStringLiteral("<!-- ants-roadmap-format: 1 -->\n") + RoadmapRender::generatedNotice();
    if (!h1.isEmpty())
        t += QStringLiteral("\n\n") + h1;
    return t;
}
QString kSlug() { return QStringLiteral("demo"); }
QString kName() { return QStringLiteral("Demo"); }

// The source store, populated per § 2.1.2's fixture table.
struct Source {
    QTemporaryDir dir;
    std::unique_ptr<RoadmapStore> store;
    qint64 projectId = 0;
    qint64 f1 = 0, f2 = 0;
};

std::unique_ptr<Source> makeSource() {
    auto f = std::make_unique<Source>();
    if (!f->dir.isValid())
        return nullptr;
    f->store = openStore(f->dir, QStringLiteral("src.sqlite"), RoadmapStore::Access::Interactive);
    if (!f->store)
        return nullptr;
    RoadmapStore &s = *f->store;
    QString err;
    const auto check = [&err](bool ok, const char *what) {
        if (!ok)
            ADD_FAILURE() << what << ": " << err.toStdString();
        return ok;
    };

    QDir().mkpath(f->dir.filePath(QStringLiteral("srcroot")));
    const auto pid = s.registerProject(f->dir.filePath(QStringLiteral("srcroot")), kName(), kSlug(), &err);
    if (!check(pid.has_value(), "registerProject"))
        return nullptr;
    f->projectId = *pid;

    // The synthetic root, exactly as the migration files the source's head.
    const auto root = s.addSection(f->projectId, QString::fromUtf8(""), QString::fromUtf8(""), 0, 0,
                                   std::nullopt, &err);
    if (!check(root.has_value(), "addSection(root)")
        || !check(s.setSectionIntro(*root, headIntro(QString::fromUtf8("# Demo — Roadmap")), &err),
                  "intro"))
        return nullptr;

    // Slugs are what the migration derives from the title (§ 2.1.2 layout 3).
    const auto work = s.addSection(f->projectId, RoadmapIndex::slugifyHeading(QStringLiteral("Work")),
                                   QStringLiteral("Work"), 2, 1, std::nullopt, &err);
    if (!check(work.has_value(), "addSection(work)"))
        return nullptr;
    const auto sub = s.addSection(f->projectId, RoadmapIndex::slugifyHeading(QStringLiteral("Sub")),
                                  QStringLiteral("Sub"), 3, 2, work, &err);
    if (!check(sub.has_value(), "addSection(sub)")
        || !check(s.setSectionIntro(*sub, QStringLiteral("Nested prose."), &err), "sub intro"))
        return nullptr;
    // An archive file: its own synthetic root, as the migration files every
    // source's head, then a section namespaced by the file's "<M>-<N>-" prefix.
    const auto archRoot = s.addSection(f->projectId, QStringLiteral("0-5"), QString::fromUtf8(""), 0, 3,
                                       std::nullopt, &err);
    if (!check(archRoot.has_value(), "addSection(archive root)")
        || !check(s.setSectionIntro(*archRoot, headIntro(), &err), "archive intro")
        || !check(s.setSectionSource(*archRoot, QStringLiteral("docs/roadmap/0.5.md"), &err),
                  "archive root source"))
        return nullptr;
    const auto arch = s.addSection(
        f->projectId, QStringLiteral("0-5-") + RoadmapIndex::slugifyHeading(QStringLiteral("Archived")),
        QStringLiteral("Archived"), 2, 4, std::nullopt, &err);
    if (!check(arch.has_value(), "addSection(archive)")
        || !check(s.setSectionSource(*arch, QStringLiteral("docs/roadmap/0.5.md"), &err), "source"))
        return nullptr;

    QJsonObject legend;
    legend.insert(QStringLiteral("planned"), QStringLiteral("Planned work."));
    legend.insert(QStringLiteral("in-progress"), QStringLiteral("In progress now."));
    legend.insert(QStringLiteral("shipped"), QStringLiteral("Done and released."));
    legend.insert(QStringLiteral("considered"), QStringLiteral("Considered, not planned."));
    // The render writes a dropped line whatever the store holds (ANTS-4977),
    // so a store that round-trips carries one.
    legend.insert(QStringLiteral("dropped"), QStringLiteral("Dropped (closed, not done)"));
    if (!check(s.setLegend(f->projectId, legend, &err), "setLegend"))
        return nullptr;

    // Carried items. F-2 is inserted first, at position 1, so insertion order
    // differs from position order.
    auto f2 = mkItem(f->projectId, QStringLiteral("F-2"), *work, 1);
    f2.kind = QStringLiteral("fix");
    f2.source = QStringLiteral("user-2026-10-02");
    f2.body = QStringLiteral("Second body prose.");
    f2.lanes = {QStringLiteral("core")};
    f2.evidence = {QStringLiteral("docs/b.png")};
    const auto f2pk = s.putItem(f2, &err);
    if (!check(f2pk.has_value(), "putItem(F-2)"))
        return nullptr;
    f->f2 = *f2pk;

    auto f1 = mkItem(f->projectId, QStringLiteral("F-1"), *work, 0);
    f1.status = QStringLiteral("in-progress");
    f1.source = QStringLiteral("in-session-2026-10-02");
    // The Dependencies: line is the converted relates-to carrier.
    f1.body = QStringLiteral("First body prose.\nDependencies: F-2.");
    f1.lanes = {QStringLiteral("vt"), QStringLiteral("chrome")};
    f1.evidence = {QStringLiteral("docs/a.png")};
    // Family 3, on a fully populated item so INV-3's every-field leg holds.
    f1.resolution = QStringLiteral("A resolution the render has no line for.");
    f1.priority = 3;
    f1.milestone = QStringLiteral("0.9.0");
    f1.extras.insert(QStringLiteral("source_kind"), QStringLiteral("bugfix"));
    const auto f1pk = s.putItem(f1, &err);
    if (!check(f1pk.has_value(), "putItem(F-1)"))
        return nullptr;
    f->f1 = *f1pk;
    if (!check(s.syncConvertedLinks(f->f1, f1.body, nullptr, &err), "syncConvertedLinks"))
        return nullptr;
    // The authored link.
    if (!check(s.relateItems(QStringLiteral("blocked-by"), f->f1, f->f2, &err), "relateItems"))
        return nullptr;

    // Family 1: neither renders. Filed LAST in the section: a hidden item's
    // position is not carried by markdown, so an element after it renumbers on
    // re-load, which is the format's and not a render loss.
    auto internal = mkItem(f->projectId, QStringLiteral("F-3"), *work, 4);
    internal.visibility = QStringLiteral("internal");
    auto dropped = mkItem(f->projectId, QStringLiteral("F-4"), *work, 5);
    dropped.status = QStringLiteral("dropped");
    if (!check(s.putItem(internal, &err).has_value(), "putItem(F-3)")
        || !check(s.putItem(dropped, &err).has_value(), "putItem(F-4)"))
        return nullptr;

    // Both element kinds; the table cell carries a literal pipe.
    if (!check(s.addElement(*work, 2, QStringLiteral("narration"),
                            QStringLiteral("A narration paragraph."), &err), "narration")
        || !check(s.addElement(*work, 3, QStringLiteral("table"),
                               QStringLiteral(R"({"header":["Lane","Note"],"rows":[["vt","x | y"]]})"),
                               &err), "table"))
        return nullptr;
    return f;
}

// --------------------------------------------------------------- projection

// § 2.1.1. Family 2 drops these record kinds wholesale; `rel` is compared.
const QSet<QString> &family2() {
    static const QSet<QString> k = {QStringLiteral("history"), QStringLiteral("citation"),
                                    QStringLiteral("feedback_ref"), QStringLiteral("id_prefix")};
    return k;
}
// Family 3 drops these keys from a surviving item. Enumerated, not a
// predicate, so a new field is a visible decision rather than a silent widening.
const QStringList &family3() {
    static const QStringList k = {
        QStringLiteral("id_origin"), QStringLiteral("provenance"), QStringLiteral("created"),
        QStringLiteral("last_modified"), QStringLiteral("shipped"), QStringLiteral("milestone"),
        QStringLiteral("resolution"), QStringLiteral("priority")};
    return k;
}
// The extras keys that hold what normalisation discarded. unresolved_links
// has a carrier and stays.
const QStringList &family3Extras() {
    static const QStringList k = {QStringLiteral("source_kind"), QStringLiteral("source_status")};
    return k;
}

struct Projected {
    QStringList lines;      // canonical NDJSON, in export order
    QVector<QJsonObject> records;
};

bool excludedItem(const QJsonObject &o) {
    return o.value(QStringLiteral("visibility")).toString() == QLatin1String("internal")
           || o.value(QStringLiteral("status")).toString() == QLatin1String("dropped");
}

Projected project(const QByteArray &ndjson) {
    QVector<QJsonObject> all;
    for (const QByteArray &line : ndjson.split('\n'))
        if (!line.trimmed().isEmpty())
            all.append(QJsonDocument::fromJson(line).object());

    // Family 1 takes an item's element and every rel naming it with it.
    QSet<QString> gone;
    for (const QJsonObject &o : all)
        if (o.value(QStringLiteral("t")).toString() == QLatin1String("item") && excludedItem(o))
            gone.insert(RoadmapParse::foldId(o.value(QStringLiteral("id")).toString()));

    Projected out;
    for (QJsonObject o : all) {
        const QString t = o.value(QStringLiteral("t")).toString();
        if (family2().contains(t))
            continue;
        if (t == QLatin1String("item")) {
            if (excludedItem(o))
                continue;
            for (const QString &k : family3())
                o.remove(k);
            QJsonObject extras = o.value(QStringLiteral("extras")).toObject();
            for (const QString &k : family3Extras())
                extras.remove(k);
            if (o.contains(QStringLiteral("extras")))
                o.insert(QStringLiteral("extras"), extras);
        } else if (t == QLatin1String("element")) {
            if (gone.contains(o.value(QStringLiteral("ref")).toString()))
                continue;
        } else if (t == QLatin1String("rel")) {
            if (gone.contains(o.value(QStringLiteral("src")).toString())
                || gone.contains(o.value(QStringLiteral("dst")).toString()))
                continue;
        }
        QByteArray bytes;
        QString err;
        if (!JsonCanonical::serialise(o, &bytes, &err))
            ADD_FAILURE() << "serialise: " << err.toStdString();
        out.lines.append(QString::fromUtf8(bytes));
        out.records.append(o);
    }
    return out;
}

// ------------------------------------------------------------------ pipeline

struct RoundTrip {
    QTemporaryDir scratch;
    std::unique_ptr<RoadmapStore> dst;
    QByteArray a, b;
    bool ran = false;
};

// § 2.1.2's six stage outcomes, asserted in pipeline order before anything
// is compared. Returns false (with a gtest failure) on the first that fails.
bool runRoundTrip(Source &src, RoundTrip &rt) {
    QString err;
    if (!rt.scratch.isValid()) {
        ADD_FAILURE() << "scratch dir";
        return false;
    }
    const QString root = rt.scratch.filePath(QStringLiteral("proj"));
    QDir().mkpath(root);

    RoadmapRender::Options ropts;
    ropts.liveRoadmapPath = QStringLiteral("ROADMAP.md");
    const auto rendered = RoadmapRender::render(*src.store, src.projectId, root, ropts, &err);
    if (!rendered) {   // 1
        ADD_FAILURE() << "render: " << err.toStdString();
        return false;
    }
    if (!rendered->gateFailures.isEmpty()) {   // 2
        ADD_FAILURE() << "INV-5 gate: " << rendered->gateFailures.join(QStringLiteral(", ")).toStdString();
        return false;
    }
    if (!rendered->committed) {   // 3
        ADD_FAILURE() << "render did not commit";
        return false;
    }
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    if (!disc) {   // 4
        ADD_FAILURE() << "findRoadmaps: " << err.toStdString();
        return false;
    }
    const RoadmapMigrate::MigrationPlan plan = RoadmapMigrate::planFrom(*disc, kName(), kSlug());

    rt.dst = openStore(rt.scratch, QStringLiteral("dst.sqlite"), RoadmapStore::Access::Bulk);
    if (!rt.dst)
        return false;
    RoadmapMigrateLoad::Options lopts;
    lopts.changedAt = QStringLiteral("2026-10-02T12:00:00Z");
    lopts.projectRoot = root;
    const auto loaded = RoadmapMigrateLoad::load(*rt.dst, plan, lopts);
    if (!loaded.ok) {   // 5
        ADD_FAILURE() << "load: " << loaded.error.toStdString();
        return false;
    }

    QBuffer ba(&rt.a), bb(&rt.b);
    ba.open(QIODevice::WriteOnly);
    bb.open(QIODevice::WriteOnly);
    if (!RoadmapExport::writeProject(*src.store, kSlug(), &ba, &err)
        || !RoadmapExport::writeProject(*rt.dst, kSlug(), &bb, &err)) {   // 6
        ADD_FAILURE() << "writeProject: " << err.toStdString();
        return false;
    }
    rt.ran = true;
    return true;
}

}  // namespace

// INV-1 — the full round trip loses nothing and invents nothing, over the
// facts markdown carries. Byte-equality of the projected line SEQUENCES.
TEST(RoadmapRoundTrip, Inv1RoundTrip) {
    auto src = makeSource();
    ASSERT_TRUE(src);
    RoundTrip rt;
    ASSERT_TRUE(runRoundTrip(*src, rt));
    const Projected a = project(rt.a);
    const Projected b = project(rt.b);
    EXPECT_EQ(a.lines, b.lines) << "A:\n" << a.lines.join(QLatin1Char('\n')).toStdString()
                                << "\nB:\n" << b.lines.join(QLatin1Char('\n')).toStdString();
}

// INV-3 — the oracle discriminates: the fixture reaches every family, both
// directions, and the comparison is order-sensitive.
TEST(RoadmapRoundTrip, Inv3OracleDiscriminates) {
    auto src = makeSource();
    ASSERT_TRUE(src);
    RoundTrip rt;
    ASSERT_TRUE(runRoundTrip(*src, rt));   // all six stage outcomes
    const Projected a = project(rt.a);
    ASSERT_FALSE(a.records.isEmpty());

    QHash<QString, int> kinds;
    for (const QJsonObject &o : a.records)
        ++kinds[o.value(QStringLiteral("t")).toString()];
    for (const char *k : {"item", "section", "element", "legend", "rel"})
        EXPECT_GT(kinds.value(QString::fromLatin1(k)), 0) << "no projected " << k << " record";

    // Every item field INV-1's Breaks-when names, on EACH projected item.
    int items = 0;
    for (const QJsonObject &o : a.records) {
        if (o.value(QStringLiteral("t")).toString() != QLatin1String("item"))
            continue;
        ++items;
        for (const char *k : {"headline", "status", "kind", "source", "layman", "body", "lanes",
                              "evidence"})
            EXPECT_TRUE(o.contains(QString::fromLatin1(k)))
                << o.value(QStringLiteral("id")).toString().toStdString() << " lacks " << k;
        // The exclusion arms ran.
        for (const QString &k : family3())
            EXPECT_FALSE(o.contains(k)) << "family-3 key survived: " << k.toStdString();
        EXPECT_FALSE(o.value(QStringLiteral("extras")).toObject().contains(QStringLiteral("source_kind")));
        EXPECT_FALSE(excludedItem(o)) << "a family-1 item survived the projection";
    }
    EXPECT_EQ(items, 2) << "both carried items, and only them";

    // The archive section's `source` key is present and non-null.
    bool archive = false;
    for (const QJsonObject &o : a.records)
        if (o.value(QStringLiteral("t")).toString() == QLatin1String("section")
            && o.value(QStringLiteral("source")).isString())
            archive = true;
    EXPECT_TRUE(archive) << "no section carries a source path";

    // The comparison is order-sensitive: the same lines reversed compare unequal.
    QStringList reversed = a.lines;
    std::reverse(reversed.begin(), reversed.end());
    EXPECT_NE(a.lines, reversed);
}
