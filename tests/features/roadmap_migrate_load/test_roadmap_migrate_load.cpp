// Feature-conformance test for ANTS-3765 INV-1..6 and INV-11..15 — the roadmap
// migration load half.
// Contract: tests/features/roadmap_migrate_load/spec.md
//
// The plans are CONSTRUCTED, never parsed. That is what makes INV-1's fault
// injectable — ANTS-3757's status vocabulary is total, so no real source file
// can produce an off-enum status — and it keeps every other test's input to the
// one property it is about.

#include <gtest/gtest.h>

#include "roadmapmigrateload.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <algorithm>
#include <climits>

namespace {

using RoadmapMigrate::MigrationPlan;
using RoadmapMigrate::PlannedElement;
using RoadmapMigrate::PlannedItem;
using RoadmapMigrate::PlannedSection;

PlannedSection section(const QString &slug, int level = 2) {
    PlannedSection s;
    s.slug = slug;
    s.title = slug;
    s.level = level;
    return s;
}

// `id` empty ⇒ the id-less shape ANTS-3757 plans for a bullet with no id token:
// no idOrigin, idAllocationOwed set, and provenance.id already `migrated`.
PlannedItem item(const QString &id, const QString &headline, const QString &slug,
                 int position) {
    PlannedItem it;
    it.id = id;
    it.status = QStringLiteral("planned");
    it.headline = headline;
    it.kind = QStringLiteral("implement");
    it.source = QStringLiteral("test");
    it.sectionSlug = slug;
    it.position = position;
    if (id.isEmpty()) {
        it.idAllocationOwed = true;
        it.provenance.insert(QStringLiteral("id"), QStringLiteral("migrated"));
    } else {
        it.idOrigin = QStringLiteral("parsed");
        it.provenance.insert(QStringLiteral("id"), QStringLiteral("asserted"));
    }
    return it;
}

MigrationPlan planOf(const QVector<PlannedItem> &items,
                     const QVector<PlannedSection> &sections = {section(QStringLiteral("s"))}) {
    MigrationPlan p;
    p.projectName = QStringLiteral("proj");
    p.exportSlug = QStringLiteral("proj");
    // ANTS-3766 § 2.1 — one Source replaces the old sourcePath/format pair.
    // Index 0 is the live roadmap, so it stores SQL NULL and never reaches
    // ANTS-3782 § 2.4's membership test.
    RoadmapMigrate::Source src;
    src.path = QStringLiteral("/nowhere/ROADMAP.md");
    src.format = QStringLiteral("ants-v1");
    p.sources.append(src);
    p.sections = sections;
    p.items = items;
    return p;
}

struct Fixture {
    QTemporaryDir dir;
    RoadmapStore store;
    QString root;

    explicit Fixture(qint64 historyCap = RoadmapStore::kDefaultHistoryCapBytes,
                     RoadmapStore::Access access = RoadmapStore::Access::Bulk)
        : store(dir.path() + QStringLiteral("/roadmap.sqlite"), historyCap, access) {
        root = dir.path() + QStringLiteral("/proj");
        QDir().mkpath(root);
    }

    RoadmapMigrateLoad::Options opts(bool dryRun = false) const {
        RoadmapMigrateLoad::Options o;
        // One stamp per migration, never per row — and the same stamp across
        // runs here on purpose: INV-14 exists because a caller doing that is
        // not misusing anything.
        o.changedAt = QStringLiteral("2026-08-01T10:00:00Z");
        o.projectRoot = root;
        o.dryRun = dryRun;
        return o;
    }

    int count(const QString &table) {
        QSqlQuery q(store.db());
        if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM ") + table) || !q.next())
            return -1;
        return q.value(0).toInt();
    }

    QString scalar(const QString &sql) {
        QSqlQuery q(store.db());
        if (!q.exec(sql) || !q.next())
            return QStringLiteral("<query failed>");
        return q.value(0).toString();
    }
};

bool hasNote(const RoadmapMigrateLoad::Outcome &o, const char *code) {
    for (const RoadmapMigrate::Note &n : o.notes) {
        if (n.code == QLatin1String(code))
            return true;
    }
    return false;
}

}  // namespace

// INV-1 / INV-11 — one project is one transaction, and the project row exists
// exactly when that project's plan committed.
TEST(RoadmapMigrateLoad, Inv1And11ProjectIsOneTransaction) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();

    // The Nth item violates a CHECK. An off-enum status is unreachable from any
    // real source file, which is exactly why the plan is built rather than
    // parsed: the fault has to be injectable to be tested at all.
    QVector<PlannedItem> items{
        item(QStringLiteral("A-1"), QStringLiteral("one"), QStringLiteral("s"), 0),
        item(QStringLiteral("A-2"), QStringLiteral("two"), QStringLiteral("s"), 1),
        item(QStringLiteral("A-3"), QStringLiteral("three"), QStringLiteral("s"), 2),
    };
    items[1].status = QStringLiteral("bogus");

    const auto bad = RoadmapMigrateLoad::load(f.store, planOf(items), f.opts());
    EXPECT_FALSE(bad.ok);
    EXPECT_FALSE(bad.error.isEmpty());
    EXPECT_TRUE(hasNote(bad, "project_refused"));

    // No row in ANY table this load would have written — including the first
    // item, which the shipped self-committing putItem() would have left behind.
    for (const QString &table : {QStringLiteral("project"), QStringLiteral("section"),
                                 QStringLiteral("item"), QStringLiteral("element"),
                                 QStringLiteral("id_prefix"), QStringLiteral("history")}) {
        EXPECT_EQ(f.count(table), 0)
            << table.toStdString() << " must be empty after a failed load";
    }

    // And the other direction: after a successful load there is exactly one
    // project row, keyed on the canonical root.
    items[1].status = QStringLiteral("planned");
    const auto good = RoadmapMigrateLoad::load(f.store, planOf(items), f.opts());
    ASSERT_TRUE(good.ok) << good.error.toStdString();
    EXPECT_EQ(f.count(QStringLiteral("project")), 1);
    EXPECT_EQ(f.count(QStringLiteral("item")), 3);
    EXPECT_EQ(good.itemsInserted, 3);
    EXPECT_EQ(f.scalar(QStringLiteral("SELECT root FROM project")).toStdString(),
              QFileInfo(f.root).canonicalFilePath().toStdString());
}

// INV-2 leg (a) — a re-run over an unchanged source of ID-BEARING items changes
// no item and writes no history.
TEST(RoadmapMigrateLoad, Inv2ReRunWithIdsIsIdempotent) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();

    const MigrationPlan p = planOf({
        item(QStringLiteral("A-1"), QStringLiteral("one"), QStringLiteral("s"), 0),
        item(QStringLiteral("A-2"), QStringLiteral("two"), QStringLiteral("s"), 1),
    });

    const auto first = RoadmapMigrateLoad::load(f.store, p, f.opts());
    ASSERT_TRUE(first.ok) << first.error.toStdString();
    EXPECT_EQ(first.itemsInserted, 2);

    const auto again = RoadmapMigrateLoad::load(f.store, p, f.opts());
    ASSERT_TRUE(again.ok) << again.error.toStdString();
    EXPECT_EQ(again.itemsInserted, 0);
    EXPECT_EQ(again.itemsUpdated, 0);
    EXPECT_EQ(again.itemsUnchanged, 2);
    EXPECT_EQ(again.itemsOrphaned, 0);
    EXPECT_EQ(f.count(QStringLiteral("history")), 0)
        << "a re-run that changed nothing must not fill the audit trail with "
           "changes that did not happen";
    EXPECT_EQ(f.count(QStringLiteral("item")), 2);
}

// INV-2 leg (b) — the leg that matters, because ~40% of the corpus is id-less.
// Matched by id alone, every id-less item is re-inserted with a freshly
// allocated id on every run and its predecessor orphaned.
TEST(RoadmapMigrateLoad, Inv2ReRunWithoutIdsIsIdempotent) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();

    const MigrationPlan p = planOf({
        item(QString(), QStringLiteral("one"), QStringLiteral("s"), 0),
        item(QString(), QStringLiteral("two"), QStringLiteral("s"), 1),
    });

    const auto first = RoadmapMigrateLoad::load(f.store, p, f.opts());
    ASSERT_TRUE(first.ok) << first.error.toStdString();
    EXPECT_EQ(first.itemsInserted, 2);
    EXPECT_EQ(first.idsAllocated, 2);
    EXPECT_TRUE(hasNote(first, "id_allocated"));

    const auto again = RoadmapMigrateLoad::load(f.store, p, f.opts());
    ASSERT_TRUE(again.ok) << again.error.toStdString();
    EXPECT_EQ(again.itemsInserted, 0)
        << "an id-less item re-read from source must match the row the last run "
           "allocated an id for, not become a second copy of it";
    EXPECT_EQ(again.itemsOrphaned, 0);
    EXPECT_EQ(again.idsAllocated, 0) << "and no id may be burnt doing it";
    EXPECT_EQ(again.itemsUnchanged, 2);
    EXPECT_EQ(f.count(QStringLiteral("item")), 2);
    EXPECT_EQ(f.count(QStringLiteral("history")), 0);
}

// Regression, found by re-running the real Ants roadmap on 2026-08-13 rather
// than by any invariant. § 2.6 matches on `(project_id, id_fold)` — the store's
// own identity — and § 2.6.1 matches an id-less item on the far weaker
// (section, headline, migrated) key. Both consume from one pool, so walking the
// plan in document order lets the weak claim take a row the strong one needs.
//
// It is reachable because § 2.8 allocates synthesised ids out of the same
// number space the project's own `.roadmap-counter` allocates from: run 1 gave
// an id-less bullet PROJ-0001, and an author later filed a real PROJ-0001. On
// re-run the id-less bullet reached that row first, and the real one fell
// through to the insert branch carrying an id the store already held — UNIQUE
// (project_id, id_fold), which rolls back the whole project. One collision cost
// every item in the plan.
TEST(RoadmapMigrateLoad, IdBearingItemClaimsItsRowBeforeAnIdlessRematch) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();

    const auto first = RoadmapMigrateLoad::load(
        f.store, planOf({item(QString(), QStringLiteral("one"), QStringLiteral("s"), 0)}),
        f.opts());
    ASSERT_TRUE(first.ok) << first.error.toStdString();
    ASSERT_EQ(first.idsAllocated, 1);
    ASSERT_EQ(f.scalar(QStringLiteral("SELECT id FROM item")),
              QStringLiteral("PROJ-0001"));

    // The id-less bullet is still id-less in source and still comes FIRST in
    // document order; the author's own PROJ-0001 arrives after it.
    const auto again = RoadmapMigrateLoad::load(
        f.store,
        planOf({item(QString(), QStringLiteral("one"), QStringLiteral("s"), 0),
                item(QStringLiteral("PROJ-0001"), QStringLiteral("filed by hand"),
                     QStringLiteral("s"), 1)}),
        f.opts());
    ASSERT_TRUE(again.ok)
        << "a duplicate id must not abort the project: " << again.error.toStdString();

    // The id owns the row it names, so PROJ-0001 is now the author's bullet...
    EXPECT_EQ(f.scalar(QStringLiteral(
                  "SELECT headline FROM item WHERE id = 'PROJ-0001'")),
              QStringLiteral("filed by hand"));
    // ...and the displaced id-less bullet is re-inserted above the high-water
    // rather than lost — the degradation § 2.6.1 already accepts.
    EXPECT_EQ(f.count(QStringLiteral("item")), 2);
    EXPECT_EQ(again.itemsInserted, 1);
    EXPECT_EQ(again.idsAllocated, 1);
    EXPECT_EQ(f.scalar(QStringLiteral(
                  "SELECT id FROM item WHERE headline = 'one'")),
              QStringLiteral("PROJ-0002"));
}

// The same collision from the other side, and the root cause of it. § 2.8 step
// 2 reads "`idHighWater()` when the row exists, OTHERWISE the plan's maximum",
// and the two terms are not alternatives: the stored row is what this store has
// allocated, the plan's maximum is what the source file already contains. A
// project files ids into ROADMAP.md between migrations without the store
// hearing about it, so the file runs ahead and the stored row alone re-issues a
// live id.
TEST(RoadmapMigrateLoad, AllocationClearsTheSourceFilesOwnIdsNotJustTheStores) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();

    const auto first = RoadmapMigrateLoad::load(
        f.store,
        planOf({item(QStringLiteral("PROJ-0005"), QStringLiteral("five"),
                     QStringLiteral("s"), 0),
                item(QString(), QStringLiteral("one"), QStringLiteral("s"), 1)}),
        f.opts());
    ASSERT_TRUE(first.ok) << first.error.toStdString();
    ASSERT_EQ(f.scalar(QStringLiteral(
                  "SELECT id FROM item WHERE headline = 'one'")),
              QStringLiteral("PROJ-0006"));
    ASSERT_EQ(f.scalar(QStringLiteral("SELECT high_water FROM id_prefix")),
              QStringLiteral("6"));

    // Between runs a human files PROJ-0007 straight into the source file, and
    // adds one more bullet with no id. The store's high-water still says 6.
    const auto again = RoadmapMigrateLoad::load(
        f.store,
        planOf({item(QStringLiteral("PROJ-0005"), QStringLiteral("five"),
                     QStringLiteral("s"), 0),
                item(QString(), QStringLiteral("one"), QStringLiteral("s"), 1),
                item(QStringLiteral("PROJ-0007"), QStringLiteral("seven"),
                     QStringLiteral("s"), 2),
                item(QString(), QStringLiteral("two"), QStringLiteral("s"), 3)}),
        f.opts());
    ASSERT_TRUE(again.ok)
        << "allocation must not re-issue an id the source file already carries: "
        << again.error.toStdString();

    EXPECT_EQ(f.scalar(QStringLiteral(
                  "SELECT id FROM item WHERE headline = 'two'")),
              QStringLiteral("PROJ-0008"))
        << "PROJ-0007 is live in the source, so the next free id is 0008";
    EXPECT_EQ(f.scalar(QStringLiteral(
                  "SELECT headline FROM item WHERE id = 'PROJ-0007'")),
              QStringLiteral("seven"));
    EXPECT_EQ(f.count(QStringLiteral("item")), 4);
}

// INV-2, the synthetic root section — regression, found by running the ten
// real project roadmaps rather than by any invariant test. A section carrying
// content above the first heading has an empty slug AND title, and the read
// half leaves both DEFAULT-CONSTRUCTED, which QSqlQuery binds as SQL NULL
// against two NOT NULL columns. Every other test here names its sections, and
// a named slug is never null.
TEST(RoadmapMigrateLoad, RootSectionWithEmptySlugLoadsAndRerunsOnce) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();

    PlannedSection root;          // slug, title: default-constructed, i.e. NULL
    root.level = 0;
    const MigrationPlan p =
        planOf({item(QStringLiteral("A-1"), QStringLiteral("above the first heading"),
                     QString(), 0)},
               {root});

    const auto first = RoadmapMigrateLoad::load(f.store, p, f.opts());
    ASSERT_TRUE(first.ok) << first.error.toStdString()
                          << " — a null slug is refused by section.slug NOT NULL";
    EXPECT_EQ(f.count(QStringLiteral("section")), 1);

    // And it must be FOUND again, not created again: `slug = NULL` is never
    // true, so a re-run would insert a second root — and SQLite's UNIQUE treats
    // NULLs as distinct, so nothing would stop it.
    const auto again = RoadmapMigrateLoad::load(f.store, p, f.opts());
    ASSERT_TRUE(again.ok) << again.error.toStdString();
    EXPECT_EQ(f.count(QStringLiteral("section")), 1)
        << "the root section must be resolved on a re-run, not duplicated";
    EXPECT_EQ(again.itemsInserted, 0);
    EXPECT_EQ(again.itemsUnchanged, 1);
}

// INV-2, the ambiguous id-less group — found by running the corpus, not by any
// leg above. § 2.6.1 refused to match when two stored items in one section
// share a headline and were both migration-allocated, on the reasoning that
// picking one of two could move history onto the wrong item. Measured against
// 3D_Engine (2026-08-01), that refusal costs more than it protects: 15 such
// items, so every re-run inserts 15 and orphans 15 — for ever, unbounded, on a
// source nobody edited. Two byte-identical headlines in one section are
// indistinguishable by every field the plan carries, so "the wrong one" is not
// an observable state; pairing them by order is.
TEST(RoadmapMigrateLoad, Inv2AmbiguousIdlessGroupPairsByOrderNotByGuessing) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();

    const MigrationPlan p = planOf({
        item(QString(), QStringLiteral("same headline"), QStringLiteral("s"), 0),
        item(QString(), QStringLiteral("same headline"), QStringLiteral("s"), 1),
        item(QString(), QStringLiteral("distinct"), QStringLiteral("s"), 2),
    });

    const auto first = RoadmapMigrateLoad::load(f.store, p, f.opts());
    ASSERT_TRUE(first.ok) << first.error.toStdString();
    EXPECT_EQ(first.itemsInserted, 3);

    const auto again = RoadmapMigrateLoad::load(f.store, p, f.opts());
    ASSERT_TRUE(again.ok) << again.error.toStdString();
    EXPECT_EQ(again.itemsInserted, 0)
        << "an ambiguous group re-inserted every run grows the store without "
           "bound on a source that never changed";
    EXPECT_EQ(again.itemsOrphaned, 0);
    EXPECT_EQ(again.idsAllocated, 0);
    EXPECT_EQ(again.itemsUnchanged, 3);
    EXPECT_EQ(f.count(QStringLiteral("item")), 3);

    // A third run, because unbounded growth is the failure and two runs cannot
    // show a trend.
    const auto third = RoadmapMigrateLoad::load(f.store, p, f.opts());
    ASSERT_TRUE(third.ok) << third.error.toStdString();
    EXPECT_EQ(f.count(QStringLiteral("item")), 3);

    // The human is still told the source has an ambiguity, because the pairing
    // is by order and order is the only thing distinguishing them.
    EXPECT_TRUE(hasNote(again, "ambiguous_rematch"));
}

// INV-3 — a re-run never clears a field the plan does not carry.
TEST(RoadmapMigrateLoad, Inv3ReRunKeepsFieldsThePlanDoesNotCarry) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();

    const MigrationPlan p =
        planOf({item(QStringLiteral("A-1"), QStringLiteral("one"), QStringLiteral("s"), 0)});
    ASSERT_TRUE(RoadmapMigrateLoad::load(f.store, p, f.opts()).ok);

    // `milestone` and not `priority`: priority is in neither setItemField()'s
    // allowlist nor QString-typed, so the obvious recipe cannot run at all.
    const qint64 pk =
        f.scalar(QStringLiteral("SELECT item_pk FROM item WHERE id = 'A-1'")).toLongLong();
    ASSERT_TRUE(f.store.setItemField(pk, QStringLiteral("milestone"),
                                     QStringLiteral("0.9.0"), &err))
        << err.toStdString();

    const auto again = RoadmapMigrateLoad::load(f.store, p, f.opts());
    ASSERT_TRUE(again.ok) << again.error.toStdString();
    EXPECT_EQ(f.scalar(QStringLiteral("SELECT milestone FROM item WHERE id = 'A-1'"))
                  .toStdString(),
              std::string("0.9.0"))
        << "a re-run writing a whole ItemWrite rather than the differing fields "
           "silently destroys every human edit";
}

// INV-4 — an item absent from source is retained, re-filed and reported.
TEST(RoadmapMigrateLoad, Inv4OrphanIsRetainedRefiledAndReported) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();

    QVector<PlannedItem> both{
        item(QStringLiteral("A-1"), QStringLiteral("one"), QStringLiteral("s"), 0),
        item(QStringLiteral("A-2"), QStringLiteral("two"), QStringLiteral("s"), 1),
    };
    ASSERT_TRUE(RoadmapMigrateLoad::load(f.store, planOf(both), f.opts()).ok);

    // The intervening run is what makes the history half testable at all: an
    // initial load writes no history, so a load-then-omit recipe would assert
    // the survival of rows that were never created.
    both[1].headline = QStringLiteral("two, edited");
    ASSERT_TRUE(RoadmapMigrateLoad::load(f.store, planOf(both), f.opts()).ok);
    ASSERT_EQ(f.count(QStringLiteral("history")), 1);

    const auto dropped = RoadmapMigrateLoad::load(
        f.store, planOf({both[0]}), f.opts());
    ASSERT_TRUE(dropped.ok) << dropped.error.toStdString();
    EXPECT_EQ(dropped.itemsOrphaned, 1);
    EXPECT_TRUE(hasNote(dropped, "orphaned_item"));

    // Its row, its history and its status survive — an id absent from source is
    // far more often a rename or an archive rotation than a deletion, and the
    // store holds history the source file never contained and cannot restore.
    EXPECT_EQ(f.count(QStringLiteral("item")), 2);
    EXPECT_EQ(f.count(QStringLiteral("history")), 1);
    EXPECT_EQ(f.scalar(QStringLiteral("SELECT status FROM item WHERE id = 'A-2'"))
                  .toStdString(),
              std::string("planned"));

    // And ANTS-3756 INV-20 still holds for BOTH items at the commit boundary:
    // the rebuild must re-file the orphan, not merely the plan's own items.
    EXPECT_EQ(f.scalar(QStringLiteral(
                           "SELECT COUNT(*) FROM item i WHERE (SELECT COUNT(*) FROM "
                           "element e WHERE e.item_pk = i.item_pk AND e.kind = 'item') != 1"))
                  .toStdString(),
              std::string("0"))
        << "every item must be filed exactly once after the rebuild";
}

// INV-5 — ordering is rebuilt without a UNIQUE collision.
TEST(RoadmapMigrateLoad, Inv5OrderingIsRebuiltNotShifted) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();

    const QVector<PlannedItem> forward{
        item(QStringLiteral("A-1"), QStringLiteral("one"), QStringLiteral("s"), 0),
        item(QStringLiteral("A-2"), QStringLiteral("two"), QStringLiteral("s"), 1),
        item(QStringLiteral("A-3"), QStringLiteral("three"), QStringLiteral("s"), 2),
    };
    ASSERT_TRUE(RoadmapMigrateLoad::load(f.store, planOf(forward), f.opts()).ok);
    const int elementsBefore = f.count(QStringLiteral("element"));

    QVector<PlannedItem> reversed{forward[2], forward[1], forward[0]};
    for (int i = 0; i < reversed.size(); ++i)
        reversed[i].position = i;
    const auto again = RoadmapMigrateLoad::load(f.store, planOf(reversed), f.opts());
    ASSERT_TRUE(again.ok) << again.error.toStdString()
                          << " — an in-place position shift fails at the first row "
                             "whose new position is still held by one that has not moved";

    EXPECT_EQ(f.count(QStringLiteral("element")), elementsBefore);
    // The ORDER BY has to be inside a subquery: SQLite applies a bare ORDER BY
    // after aggregation, so it would not order what group_concat() concatenates.
    EXPECT_EQ(f.scalar(QStringLiteral(
                           "SELECT group_concat(id, ',') FROM "
                           "(SELECT i.id FROM element e JOIN item i "
                           " ON i.item_pk = e.item_pk WHERE e.kind = 'item' "
                           " ORDER BY e.position)"))
                  .toStdString(),
              std::string("A-3,A-2,A-1"));
    // Exactly 0..n-1, no gaps and no duplicates.
    EXPECT_EQ(f.scalar(QStringLiteral(
                           "SELECT group_concat(position, ',') FROM "
                           "(SELECT DISTINCT position FROM element ORDER BY position)"))
                  .toStdString(),
              std::string("0,1,2"));
}

// INV-6 — a rolled-back load allocates no id. Two legs, and the expected value
// is stated in both: "asserts the high-water" is satisfiable by a test that
// asserts nothing.
TEST(RoadmapMigrateLoad, Inv6RolledBackLoadAllocatesNoId) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();

    QVector<PlannedItem> items{
        item(QString(), QStringLiteral("one"), QStringLiteral("s"), 0),
        item(QString(), QStringLiteral("two"), QStringLiteral("s"), 1),
    };
    items[1].status = QStringLiteral("bogus");   // fails after the first allocation

    // Leg 1, the first run: no id_prefix row at all.
    const auto failed = RoadmapMigrateLoad::load(f.store, planOf(items), f.opts());
    ASSERT_FALSE(failed.ok);
    EXPECT_EQ(f.count(QStringLiteral("id_prefix")), 0)
        << "an id burnt by a failed run is an id that exists in no document and "
           "blocks a future one";

    // Leg 2, a re-run: exactly the pre-run high-water. The project has to exist
    // for a counter to be seeded against it, so a good load comes first.
    items[1].status = QStringLiteral("planned");
    const auto ok = RoadmapMigrateLoad::load(f.store, planOf(items), f.opts());
    ASSERT_TRUE(ok.ok) << ok.error.toStdString();
    const QString prefix = f.scalar(QStringLiteral("SELECT prefix FROM id_prefix"));
    ASSERT_TRUE(f.store.raiseIdHighWater(ok.projectId, prefix, 41, &err))
        << err.toStdString();

    QVector<PlannedItem> more{items[0], items[1],
                             item(QString(), QStringLiteral("three"),
                                  QStringLiteral("s"), 2)};
    more[2].status = QStringLiteral("bogus");
    const auto failedAgain = RoadmapMigrateLoad::load(f.store, planOf(more), f.opts());
    ASSERT_FALSE(failedAgain.ok);
    EXPECT_EQ(f.scalar(QStringLiteral("SELECT high_water FROM id_prefix")).toStdString(),
              std::string("41"));
}

// INV-12 — a load against an Interactive store is refused.
TEST(RoadmapMigrateLoad, Inv12InteractiveStoreIsRefused) {
    Fixture f(RoadmapStore::kDefaultHistoryCapBytes, RoadmapStore::Access::Interactive);
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();

    const auto refused = RoadmapMigrateLoad::load(
        f.store,
        planOf({item(QStringLiteral("A-1"), QStringLiteral("one"), QStringLiteral("s"), 0)}),
        f.opts());
    EXPECT_FALSE(refused.ok)
        << "a 5 s deadline against a migration-sized transaction fails SOMETIMES, "
           "which is worse than failing always";
    EXPECT_EQ(f.count(QStringLiteral("project")), 0);
    EXPECT_EQ(f.count(QStringLiteral("item")), 0);
}

// INV-13 — dryRun writes nothing and reports what a real run would have done.
TEST(RoadmapMigrateLoad, Inv13DryRunReportsTheRealRun) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();

    QVector<PlannedSection> sections{section(QStringLiteral("s"))};
    MigrationPlan p = planOf({item(QStringLiteral("A-1"), QStringLiteral("one"),
                                   QStringLiteral("s"), 0),
                              item(QString(), QStringLiteral("two"),
                                   QStringLiteral("s"), 1)},
                             sections);
    PlannedElement narration;
    narration.kind = QStringLiteral("narration");
    narration.payload = QStringLiteral("some prose");
    narration.sectionSlug = QStringLiteral("s");
    narration.position = 2;
    p.elements.push_back(narration);

    const auto dry = RoadmapMigrateLoad::load(f.store, p, f.opts(/*dryRun=*/true));
    ASSERT_TRUE(dry.ok) << dry.error.toStdString();
    for (const QString &table : {QStringLiteral("project"), QStringLiteral("section"),
                                 QStringLiteral("item"), QStringLiteral("element"),
                                 QStringLiteral("id_prefix")}) {
        EXPECT_EQ(f.count(table), 0)
            << table.toStdString() << " must be untouched by a dry run";
    }

    const auto real = RoadmapMigrateLoad::load(f.store, p, f.opts());
    ASSERT_TRUE(real.ok) << real.error.toStdString();

    // Count by count, EXCLUDING projectId — that is a rowid the dry run rolled
    // back, so any equality between the two would pass or fail on SQLite's rowid
    // reuse rather than on this invariant.
    EXPECT_EQ(dry.itemsInserted, real.itemsInserted);
    EXPECT_EQ(dry.itemsUpdated, real.itemsUpdated);
    EXPECT_EQ(dry.itemsUnchanged, real.itemsUnchanged);
    EXPECT_EQ(dry.itemsOrphaned, real.itemsOrphaned);
    EXPECT_EQ(dry.idsAllocated, real.idsAllocated);
    EXPECT_EQ(dry.sectionsWritten, real.sectionsWritten);
    EXPECT_EQ(dry.elementsWritten, real.elementsWritten);
    EXPECT_EQ(dry.historyRows, real.historyRows);
    EXPECT_EQ(dry.notes.size(), real.notes.size());
    EXPECT_GT(real.elementsWritten, 0) << "a dry run that short-circuits before "
                                          "the constraint-bearing writes is a "
                                          "syntax check wearing an atomicity "
                                          "check's clothes";
}

// INV-14 — a re-run's history rows never collide, even when two runs are given
// the SAME stamp. The trigger is a caller doing something entirely reasonable.
TEST(RoadmapMigrateLoad, Inv14HistorySeqContinuesAcrossRunsWithOneStamp) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();

    QVector<PlannedItem> items{
        item(QStringLiteral("A-1"), QStringLiteral("one"), QStringLiteral("s"), 0)};
    ASSERT_TRUE(RoadmapMigrateLoad::load(f.store, planOf(items), f.opts()).ok);

    items[0].headline = QStringLiteral("edited once");
    const auto second = RoadmapMigrateLoad::load(f.store, planOf(items), f.opts());
    ASSERT_TRUE(second.ok) << second.error.toStdString();
    EXPECT_EQ(second.itemsUpdated, 1);

    items[0].headline = QStringLiteral("edited twice");
    const auto third = RoadmapMigrateLoad::load(f.store, planOf(items), f.opts());
    ASSERT_TRUE(third.ok)
        << third.error.toStdString()
        << " — seq restarting at 0 per run collides on UNIQUE (item_pk, "
           "changed_at, seq) and aborts the whole project";

    EXPECT_EQ(f.count(QStringLiteral("history")), 2);
    EXPECT_EQ(f.scalar(QStringLiteral(
                           "SELECT group_concat(seq, ',') FROM "
                           "(SELECT seq FROM history ORDER BY seq)"))
                  .toStdString(),
              std::string("0,1"));
}

// INV-15 — a history write refused at the cap does not abort the project. The
// only exception to INV-1, and asserted rather than merely written down.
TEST(RoadmapMigrateLoad, Inv15HistoryCapDoesNotAbortTheProject) {
    Fixture f(8);   // tiny: the first re-run update crosses it
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();

    QVector<PlannedItem> items{
        item(QStringLiteral("A-1"), QStringLiteral("one"), QStringLiteral("s"), 0)};
    ASSERT_TRUE(RoadmapMigrateLoad::load(f.store, planOf(items), f.opts()).ok);

    items[0].headline = QStringLiteral("edited");
    const auto capped = RoadmapMigrateLoad::load(f.store, planOf(items), f.opts());
    EXPECT_TRUE(capped.ok)
        << capped.error.toStdString()
        << " — losing a project's whole migration because its audit trail is "
           "full inverts the priority between the data and the record of it";
    EXPECT_TRUE(hasNote(capped, "history_capped"));
    EXPECT_EQ(f.scalar(QStringLiteral("SELECT headline FROM item WHERE id = 'A-1'"))
                  .toStdString(),
              std::string("edited"))
        << "the item update the refused history row accompanies must commit";
    EXPECT_EQ(f.count(QStringLiteral("history")), 0);
}

// ============================================================================
// ANTS-3766 INV-9 / INV-10 and ANTS-3782 INV-14 / INV-28.
// These are the legs that need a STORE, which is why they live here rather
// than in roadmap_migrate_read.
// ============================================================================

namespace {

QString archivesDir() {
    return QString::fromUtf8(ANTS_MIGRATE_FIXTURE_DIR) + QStringLiteral("/archives");
}

// A committed archive root copied into a temp directory. INV-9 and INV-4
// MUTATE their fixture (both append to the live file between two plans), and a
// test that edits a committed fixture in place corrupts every later assertion
// in the same run and shows up as a dirty working tree.
struct CopiedRoot {
    QTemporaryDir dir;
    QString root;
    explicit CopiedRoot(const char *name) {
        root = dir.path() + QStringLiteral("/root");
        const QString src = archivesDir() + QLatin1Char('/') + QString::fromUtf8(name);
        QDirIterator walk(src, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (walk.hasNext()) {
            const QString from = walk.next();
            const QString to = root + QLatin1Char('/') + QDir(src).relativeFilePath(from);
            QDir().mkpath(QFileInfo(to).absolutePath());
            QFile::copy(from, to);
        }
    }
    void appendToLiveRoadmap(const QString &text) const {
        QFile f(root + QStringLiteral("/ROADMAP.md"));
        EXPECT_TRUE(f.open(QIODevice::Append | QIODevice::Text));
        f.write(text.toUtf8());
    }
    MigrationPlan plan() const {
        QString err;
        const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
        EXPECT_TRUE(disc.has_value()) << err.toStdString();
        if (!disc) return {};
        return RoadmapMigrate::planFrom(*disc, QStringLiteral("arch"),
                                        QStringLiteral("arch"));
    }
};

// A source_path read straight out of SQL, so the assertion's oracle is the
// column rather than the writer's idea of it.
QString sqlSourcePath(RoadmapStore &store, const QString &slug) {
    QSqlQuery q(store.db());
    q.prepare(QStringLiteral("SELECT source_path FROM section WHERE slug = ?"));
    q.addBindValue(slug);
    if (!q.exec() || !q.next()) return QStringLiteral("<no row>");
    return q.value(0).isNull() ? QStringLiteral("<NULL>") : q.value(0).toString();
}

}  // namespace

// --------------------------------------------------------- ANTS-3766 INV-9 --
// Archive items survive an edit to the LIVE file. The edit BETWEEN the two
// loads is the whole invariant: stated as "load the same input twice" this is
// vacuous, because a counter-based slug scheme is perfectly deterministic and
// reproduces its own slugs exactly on an unchanged re-run — it would pass
// against the very design ANTS-3766 § 2.3.1 rejects.
TEST(roadmap_migrate_load, Ants3766Inv9ArchiveItemsSurviveALiveEdit) {
    const CopiedRoot src("baseline");
    QTemporaryDir dbDir;
    RoadmapStore store(dbDir.path() + QStringLiteral("/roadmap.sqlite"),
                       RoadmapStore::kDefaultHistoryCapBytes,
                       RoadmapStore::Access::Bulk);
    QString err;
    ASSERT_TRUE(store.open(&err)) << err.toStdString();

    RoadmapMigrateLoad::Options o;
    o.changedAt = QStringLiteral("2026-08-01T10:00:00Z");
    o.projectRoot = src.root;

    const MigrationPlan first = src.plan();
    ASSERT_EQ(first.sources.size(), 3);
    const auto r1 = RoadmapMigrateLoad::load(store, first, o);
    ASSERT_TRUE(r1.ok) << r1.error.toStdString();
    ASSERT_GT(r1.itemsInserted, 0);

    // Which item ids came from an archive, so the second run's outcome can be
    // read restricted to them.
    QSet<QString> archiveIds;
    for (const auto &it : first.items)
        if (it.sourceIndex >= 1 && !it.id.isEmpty()) archiveIds.insert(it.id.toLower());
    ASSERT_FALSE(archiveIds.isEmpty()) << "0.6.md carries ANTS-1001 and ANTS-1002";

    src.appendToLiveRoadmap(QStringLiteral(
        "\n### ⚡ Performance\n\n"
        "- 📋 [BASE-0007] **A fourth live perf item.**\n  Kind: perf.\n"));

    const MigrationPlan second = src.plan();
    const auto r2 = RoadmapMigrateLoad::load(store, second, o);
    ASSERT_TRUE(r2.ok) << r2.error.toStdString();

    // Every archive item must still be the row the first load wrote.
    for (const QString &id : std::as_const(archiveIds)) {
        QSqlQuery q(store.db());
        q.prepare(QStringLiteral("SELECT COUNT(*) FROM item WHERE id_fold = ?"));
        q.addBindValue(id);
        ASSERT_TRUE(q.exec() && q.next());
        EXPECT_EQ(q.value(0).toInt(), 1)
            << "INV-9: " << id.toStdString()
            << " — breaks when archive slugs depend on live-file content "
               "(§ 2.3.1), which re-files the affected section's items under a "
               "shifted slug and orphans the originals. This is the end-to-end "
               "detector for INV-4";
    }
    EXPECT_EQ(r2.itemsInserted, 1)
        << "INV-9: exactly the one item the live edit added — no archive item "
           "is re-inserted";
}

// -------------------------------------------------------- ANTS-3766 INV-10 --
// A plan carrying archive_slug_collision is REFUSED before anything is written.
TEST(roadmap_migrate_load, Ants3766Inv10CollisionPlanIsRefused) {
    const CopiedRoot src("collision");
    QTemporaryDir dbDir;
    RoadmapStore store(dbDir.path() + QStringLiteral("/roadmap.sqlite"),
                       RoadmapStore::kDefaultHistoryCapBytes,
                       RoadmapStore::Access::Bulk);
    QString err;
    ASSERT_TRUE(store.open(&err)) << err.toStdString();

    RoadmapMigrateLoad::Options o;
    o.changedAt = QStringLiteral("2026-08-01T10:00:00Z");
    o.projectRoot = src.root;

    const MigrationPlan plan = src.plan();
    const auto out = RoadmapMigrateLoad::load(store, plan, o);
    EXPECT_FALSE(out.ok)
        << "INV-10: breaks when the note is dropped and the duplicate reaches "
           "the store, which SILENTLY MERGES the archive section into the live "
           "one — UNIQUE (project_id, slug) never fires, because § 2.6.1 "
           "resolves every section with findSection() and calls addSection() "
           "only for a genuinely-new slug. A clause asserting an ABORT would "
           "describe a failure that cannot happen and would pass against the "
           "merge it is meant to catch";
    for (const QString &t : {QStringLiteral("project"), QStringLiteral("section"),
                             QStringLiteral("item")}) {
        QSqlQuery q(store.db());
        ASSERT_TRUE(q.exec(QStringLiteral("SELECT COUNT(*) FROM ") + t) && q.next());
        EXPECT_EQ(q.value(0).toInt(), 0) << t.toStdString() << " must be empty";
    }
}

// -------------------------------------------------------- ANTS-3782 INV-14 --
// The persisted discriminator is correct AND machine-independent.
TEST(roadmap_migrate_load, Ants3782Inv14SourcePathIsRootRelative) {
    const CopiedRoot src("baseline");
    QTemporaryDir dbDir;
    RoadmapStore store(dbDir.path() + QStringLiteral("/roadmap.sqlite"),
                       RoadmapStore::kDefaultHistoryCapBytes,
                       RoadmapStore::Access::Bulk);
    QString err;
    ASSERT_TRUE(store.open(&err)) << err.toStdString();

    RoadmapMigrateLoad::Options o;
    o.changedAt = QStringLiteral("2026-08-01T10:00:00Z");
    o.projectRoot = src.root;

    const MigrationPlan plan = src.plan();
    ASSERT_EQ(plan.sources.size(), 3);
    const auto out = RoadmapMigrateLoad::load(store, plan, o);
    ASSERT_TRUE(out.ok) << out.error.toStdString();

    // Read back through readSection(), the typed path — INV-26's surface.
    int liveSections = 0, archiveSections = 0;
    for (const auto &s : plan.sections) {
        // The synthetic root's slug is a DEFAULT-CONSTRUCTED QString, which is
        // null rather than empty; the loader stores it through notNull(), so a
        // lookup has to normalise the same way or it misses that one row.
        const QString slug = s.slug.isNull() ? QString::fromUtf8("") : s.slug;
        const auto sid = store.findSection(out.projectId, slug, &err);
        ASSERT_TRUE(sid.has_value()) << slug.toStdString() << ": " << err.toStdString();
        const auto row = store.readSection(*sid, &err);
        ASSERT_TRUE(row.has_value()) << err.toStdString();
        if (s.sourceIndex == 0) {
            ++liveSections;
            EXPECT_FALSE(row->sourcePath.has_value())
                << "INV-14: a live section stores SQL NULL, not a path — " 
                << s.slug.toStdString();
        } else {
            ++archiveSections;
            ASSERT_TRUE(row->sourcePath.has_value()) << s.slug.toStdString();
            const QString want = QStringLiteral("docs/roadmap/") +
                                 QFileInfo(plan.sources.at(s.sourceIndex).path).fileName();
            EXPECT_EQ(*row->sourcePath, want)
                << "INV-14: breaks when load() stores sources[sourceIndex].path "
                   "VERBATIM, which is absolute — the store then works only on "
                   "the machine that wrote it and § 2.5's membership test never "
                   "matches anywhere else";
        }
    }
    EXPECT_GT(liveSections, 0);
    EXPECT_GT(archiveSections, 0) << "0.6.md contributes sections";
    EXPECT_EQ(sqlSourcePath(store, QStringLiteral("0-6-features")),
              QStringLiteral("docs/roadmap/0.6.md"));
}

// --------------------------------------------------------- ANTS-3815 INV-2 --
// The migration records SOURCE INDEX 0's format and no other.
//
// Leg (a). The plan is BUILT, with two sources whose formats DIFFER, so the
// assertion can distinguish index 0 from a plan-level or last-source read. It
// proves the write reads index 0 and nothing more — the test wrote the plan, so
// it cannot also prove index 0 is the live roadmap. That is leg (b)'s job.
TEST(roadmap_migrate_load, Ants3815Inv2aRecordsSourceZerosFormat) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();

    MigrationPlan p = planOf({item(QStringLiteral("A-1"), QStringLiteral("one"),
                                   QStringLiteral("s"), 0)});
    ASSERT_EQ(p.sources.size(), 1);
    ASSERT_EQ(p.sources.at(0).format.toStdString(), std::string("ants-v1"));
    // The archive must be a REAL file under the fixture's root: ANTS-3782 § 2.4
    // resolves each source through canonicalFilePath(), which is empty for a path
    // that does not exist, and refuses one it cannot place — so an invented path
    // aborts the load before this invariant is reached.
    const QString archivePath = f.root + QStringLiteral("/docs/roadmap/0.6.md");
    ASSERT_TRUE(QDir().mkpath(QFileInfo(archivePath).absolutePath()));
    {
        QFile a(archivePath);
        ASSERT_TRUE(a.open(QIODevice::WriteOnly | QIODevice::Truncate));
        a.write("# 0.6\n\n- [x] An archived item\n");
    }
    RoadmapMigrate::Source archive;
    archive.path = archivePath;
    archive.format = QStringLiteral("github-task-list");
    p.sources.append(archive);

    const auto out = RoadmapMigrateLoad::load(f.store, p, f.opts());
    ASSERT_TRUE(out.ok) << out.error.toStdString();

    const auto row = f.store.readProject(out.projectId, &err);
    ASSERT_TRUE(row.has_value()) << err.toStdString();
    EXPECT_EQ(row->sourceFormat.toStdString(), std::string("ants-v1"))
        << "INV-2: breaks when the write is keyed off a plan-level or last-source "
           "format, which silently records an ARCHIVE's grammar for the whole "
           "project";
}

// Leg (b). The other half of the precondition: source index 0 really is the live
// roadmap in a plan built from real discovery. The assertion is on the PLAN, not
// on Discovery — Discovery already documents the guarantee, so a leg stopping
// there would test the type that was never in doubt; what is untested is the
// plan builder's order preservation, which is what leg (a) rests on.
TEST(roadmap_migrate_load, Ants3815Inv2bPlanSourceZeroIsTheLiveRoadmap) {
    const CopiedRoot src("baseline");
    const MigrationPlan plan = src.plan();
    ASSERT_GT(plan.sources.size(), 1) << "the fixture must carry archives too";
    EXPECT_EQ(QFileInfo(plan.sources.at(0).path).canonicalFilePath().toStdString(),
              QFileInfo(src.root + QStringLiteral("/ROADMAP.md"))
                  .canonicalFilePath().toStdString())
        << "INV-2: breaks when planFrom() reorders sources — leg (a) stays green "
           "and the stored value becomes an archive's";
}

// --------------------------------------------------------- ANTS-3815 INV-7 --
// Every refusal on the write path aborts the load and names its cause.
TEST(roadmap_migrate_load, Ants3815Inv7SourceFormatWriteRefusals) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();

    // A committed project to aim the valid cases at.
    const auto seeded = RoadmapMigrateLoad::load(
        f.store, planOf({item(QStringLiteral("A-1"), QStringLiteral("one"),
                              QStringLiteral("s"), 0)}), f.opts());
    ASSERT_TRUE(seeded.ok) << seeded.error.toStdString();

    // (1) An unknown projectId. THE case that earns numRowsAffected(): an UPDATE
    // matching no row SUCCEEDS in SQLite, so a setter returning the bare exec()
    // result reports having written a project that does not exist — ANTS-3767's
    // failure mode one column along.
    err.clear();
    EXPECT_FALSE(f.store.setProjectSourceFormat(999999, QStringLiteral("ants-v1"), &err));
    EXPECT_FALSE(err.isEmpty()) << "a refusal must carry its reason";

    // (2) A format outside § 2.1's CHECK set.
    err.clear();
    EXPECT_FALSE(f.store.setProjectSourceFormat(seeded.projectId,
                                                QStringLiteral("klingon"), &err));
    EXPECT_FALSE(err.isEmpty());

    // (3) A plan with no sources — refused BEFORE the loader indexes them.
    // QVector::at() is unchecked in a release build, so this guard is what stands
    // between an empty plan and undefined behaviour.
    MigrationPlan empty = planOf({item(QStringLiteral("B-1"), QStringLiteral("two"),
                                       QStringLiteral("s"), 0)});
    empty.sources.clear();
    const auto out = RoadmapMigrateLoad::load(f.store, empty, f.opts());
    EXPECT_FALSE(out.ok);
    EXPECT_FALSE(out.error.isEmpty());
    // And it did not commit: the only project row is the seeded one.
    EXPECT_EQ(f.count(QStringLiteral("project")), 1)
        << "a refused load must leave no project row behind";
    EXPECT_EQ(f.scalar(QStringLiteral(
                  "SELECT COUNT(*) FROM item WHERE id = 'B-1'")).toStdString(),
              std::string("0"));

    // (4) A plan whose LIVE source carries an empty format. § 2.1's CHECK admits
    // '' on purpose — it is what a version-1 row takes when the rung runs — so
    // nothing below Loader::run() would stop this, and it is the one input that
    // manufactures a freshly-migrated row indistinguishable from a pre-bump one.
    // § 2.4 then dispatches that project as version 1 forever, leaving INV-6's
    // drift refusal unreachable for it with no error anywhere.
    //
    // findRoadmaps() cannot produce this; a caller-built plan can, and load()
    // cannot tell the two apart. That is precisely why the guard is in the
    // loader and not left to the column.
    MigrationPlan blank = planOf({item(QStringLiteral("C-1"), QStringLiteral("three"),
                                       QStringLiteral("s"), 0)});
    blank.sources[0].format = QString::fromUtf8("");
    const auto blankOut = RoadmapMigrateLoad::load(f.store, blank, f.opts());
    EXPECT_FALSE(blankOut.ok)
        << "a migration may not record '' — it means \"not recorded\"";
    EXPECT_FALSE(blankOut.error.isEmpty()) << "a refusal must carry its reason";
    EXPECT_EQ(f.scalar(QStringLiteral(
                  "SELECT COUNT(*) FROM item WHERE id = 'C-1'")).toStdString(),
              std::string("0"));
    // The seeded project's recorded format is untouched by the refused load.
    const auto seededRow = f.store.readProject(seeded.projectId, &err);
    ASSERT_TRUE(seededRow.has_value()) << err.toStdString();
    EXPECT_EQ(seededRow->sourceFormat.toStdString(), std::string("ants-v1"));
}

// -------------------------------------------------------- ANTS-3796 INV-4 --
// The migration assigns positions that are a permutation of 0 … n-1 over the
// sections a run's plan names, in document order across all its sources.
//
// Scoped to the PLAN's sections because § 2.3.1 keeps a heading deleted from a
// re-run's source, stale position and all: a whole-table permutation assertion
// would be false on any re-run that dropped a heading, which is an ordinary
// re-run and not an error. This fixture is a first load, so the two sets
// coincide — the scoping is stated so a later re-run leg cannot be added
// against a claim that was never made.
TEST(roadmap_migrate_load, Ants3796Inv4PositionsAreADocumentOrderPermutation) {
    const CopiedRoot src("baseline");
    QTemporaryDir dbDir;
    RoadmapStore store(dbDir.path() + QStringLiteral("/roadmap.sqlite"),
                       RoadmapStore::kDefaultHistoryCapBytes,
                       RoadmapStore::Access::Bulk);
    QString err;
    ASSERT_TRUE(store.open(&err)) << err.toStdString();

    RoadmapMigrateLoad::Options o;
    o.changedAt = QStringLiteral("2026-08-01T10:00:00Z");
    o.projectRoot = src.root;

    const MigrationPlan plan = src.plan();
    ASSERT_EQ(plan.sources.size(), 3);
    const auto out = RoadmapMigrateLoad::load(store, plan, o);
    ASSERT_TRUE(out.ok) << out.error.toStdString();

    // Read back through the typed enumerator, which is what makes the sort key
    // reachable at all — the alternative is SELECT section_id in raw SQL plus a
    // point lookup per row, the reach-past-the-reader § 2.3.1 exists to stop.
    const auto rows = store.listSections(out.projectId, &err);
    ASSERT_TRUE(rows.has_value()) << err.toStdString();
    ASSERT_EQ(rows->size(), plan.sections.size())
        << "a first load writes exactly the plan's sections";

    // Leg 1 — DENSE: the positions are a permutation of 0 … n-1. Asserted here
    // rather than left to a UNIQUE constraint (§ 2.1 declines one), because the
    // reachable bug is a loader that numbers PER SOURCE and restarts at 0 for
    // each archive — which produces duplicates that § 2.2's tie-break then
    // hides behind a plausible slug order.
    QList<int> positions;
    for (const RoadmapStore::SectionRow &r : std::as_const(*rows))
        positions << r.position;
    std::sort(positions.begin(), positions.end());
    QList<int> dense;
    for (int i = 0; i < rows->size(); ++i)
        dense << i;
    EXPECT_EQ(positions, dense)
        << "INV-4: positions are not a permutation of 0 … n-1 — the loader is numbering per "
           "source rather than across the whole plan";

    // Leg 2 — every LIVE-roadmap section precedes every archive section. Index
    // 0 IS the live roadmap (roadmapmigrate.h), so this is a real ordering
    // claim and not the vacuous "an earlier-indexed archive comes first".
    int liveMax = -1, archiveMin = INT_MAX;
    QMap<int, QList<int>> bySource;   // sourceIndex -> that source's positions
    for (const PlannedSection &s : plan.sections) {
        const QString slug = s.slug.isNull() ? QString::fromUtf8("") : s.slug;
        const auto sid = store.findSection(out.projectId, slug, &err);
        ASSERT_TRUE(sid.has_value()) << slug.toStdString() << ": " << err.toStdString();
        const auto row = store.readSection(*sid, &err);
        ASSERT_TRUE(row.has_value()) << err.toStdString();
        if (s.sourceIndex == 0)
            liveMax = std::max(liveMax, row->position);
        else
            archiveMin = std::min(archiveMin, row->position);
        bySource[s.sourceIndex] << row->position;
    }
    ASSERT_GT(liveMax, -1) << "the live roadmap contributes sections";
    ASSERT_LT(archiveMin, INT_MAX) << "0.6.md contributes sections";
    EXPECT_LT(liveMax, archiveMin)
        << "INV-4: an archive section sorted before a live-roadmap one — breaks when the "
           "ordinal is walked in plan order rather than (sourceIndex, firstLine)";

    // Leg 3 — archive sections follow one another in sourceIndex order, so a
    // second rotated file cannot interleave with the first.
    int previousMax = -1;
    for (auto it = bySource.constBegin(); it != bySource.constEnd(); ++it) {
        const auto [lo, hi] = std::minmax_element(it->constBegin(), it->constEnd());
        EXPECT_GT(*lo, previousMax)
            << "source " << it.key() << " interleaves with an earlier source";
        previousMax = *hi;
    }
}

// The same fixture through a differently-spelled but EQUIVALENT root stores
// byte-identical values. The symlinked-root leg is the only detector for a
// half-canonicalised conversion; the trailing-slash leg passes against it,
// because QDir normalises that much on its own.
TEST(roadmap_migrate_load, Ants3782Inv14EquivalentRootSpellingsAgree) {
    const CopiedRoot src("baseline");

    const auto storedFor = [&](const QString &rootSpelling) {
        QTemporaryDir dbDir;
        RoadmapStore store(dbDir.path() + QStringLiteral("/roadmap.sqlite"),
                           RoadmapStore::kDefaultHistoryCapBytes,
                           RoadmapStore::Access::Bulk);
        QString err;
        EXPECT_TRUE(store.open(&err)) << err.toStdString();
        RoadmapMigrateLoad::Options o;
        o.changedAt = QStringLiteral("2026-08-01T10:00:00Z");
        o.projectRoot = rootSpelling;
        QString derr;
        const auto disc = RoadmapMigrate::findRoadmaps(rootSpelling, &derr);
        EXPECT_TRUE(disc.has_value()) << derr.toStdString();
        if (!disc) return QStringLiteral("<no discovery>");
        const auto plan = RoadmapMigrate::planFrom(*disc, QStringLiteral("a"),
                                                   QStringLiteral("a"));
        const auto out = RoadmapMigrateLoad::load(store, plan, o);
        EXPECT_TRUE(out.ok) << out.error.toStdString();
        return sqlSourcePath(store, QStringLiteral("0-6-features"));
    };

    const QString plain = storedFor(src.root);
    EXPECT_EQ(plain, QStringLiteral("docs/roadmap/0.6.md"));

    EXPECT_EQ(storedFor(src.root + QLatin1Char('/')), plain)
        << "INV-14: a trailing slash must not change the stored value — and "
           "this leg PASSES against a half-canonicalised conversion, which is "
           "why it cannot be the only one";

    // A path through a symlink: absoluteFilePath() does not resolve one, so
    // canonicalising only ONE side computes a path OUT of the project.
    const QString link = src.dir.path() + QStringLiteral("/link");
    ASSERT_TRUE(QFile::link(src.root, link)) << "could not create the symlink";
    EXPECT_EQ(storedFor(link), plain)
        << "INV-14: breaks when EITHER side of the conversion is left "
           "un-canonicalised — canonicalise only the root and the result is a "
           "path computed OUT of the project (../link/docs/…); canonicalise "
           "only the source and the mirror defect appears. This leg is the only "
           "detector for both";
}

// -------------------------------------------------------- ANTS-3782 INV-28 --
// A source whose stored value would be unplaceable REFUSES the project.
//
// Each leg CONSTRUCTS the plan directly rather than planning a fixture root:
// none of the three shapes is reachable through findRoadmaps(), which
// enumerates only docs/roadmap/ entries matching its regex and rejects
// symlinks. They survive because load() takes a MigrationPlan and cannot tell
// which producer built it — the same technique INV-1 above uses for its
// off-enum status, and for the same reason.
TEST(roadmap_migrate_load, Ants3782Inv28UnplaceableSourceRefusesTheProject) {
    struct Leg {
        const char *what;
        QString sourcePath;                 // relative to the root, or absolute
        bool absolute;
    };

    QTemporaryDir outer;
    const QString root = outer.path() + QStringLiteral("/proj");
    QDir().mkpath(root + QStringLiteral("/docs/roadmap"));
    QDir().mkpath(root + QStringLiteral("/docs/archive"));
    QDir().mkpath(outer.path() + QStringLiteral("/outside"));
    QFile::copy(archivesDir() + QStringLiteral("/baseline/docs/roadmap/0.6.md"),
                root + QStringLiteral("/docs/archive/0.6.md"));
    QFile::copy(archivesDir() + QStringLiteral("/baseline/docs/roadmap/0.6.md"),
                outer.path() + QStringLiteral("/outside/0.6.md"));

    const QVector<Leg> legs = {
        // Leg 1 — a source whose canonicalFilePath() is EMPTY. Qt returns the
        // empty string for a path that does not resolve; in production this is
        // a source deleted between discovery and load.
        {"a source that does not resolve",
         root + QStringLiteral("/docs/roadmap/gone-0.6.md"), true},
        // Leg 2 — canonicalises OUTSIDE the root, yielding a ../ value § 2.5's
        // membership test can never match.
        {"a source outside the project root",
         outer.path() + QStringLiteral("/outside/0.6.md"), true},
        // Leg 3 — INSIDE the root but outside docs/roadmap/. Stores cleanly,
        // matches nothing, and is the same silently-unplaceable section by a
        // quieter route.
        {"a source inside the root but outside docs/roadmap/",
         root + QStringLiteral("/docs/archive/0.6.md"), true},
    };

    for (const Leg &leg : legs) {
        QTemporaryDir dbDir;
        RoadmapStore store(dbDir.path() + QStringLiteral("/roadmap.sqlite"),
                           RoadmapStore::kDefaultHistoryCapBytes,
                           RoadmapStore::Access::Bulk);
        QString err;
        ASSERT_TRUE(store.open(&err)) << err.toStdString();

        MigrationPlan p = planOf({item(QStringLiteral("A-1"),
                                       QStringLiteral("An item."),
                                       QStringLiteral("s"), 0)});
        // planOf() supplies sources[0], the live roadmap; this is the archive
        // whose converted value the guard must reject.
        RoadmapMigrate::Source arc;
        arc.path = leg.sourcePath;
        arc.format = QStringLiteral("ants-v1");
        p.sources.append(arc);

        RoadmapMigrateLoad::Options o;
        o.changedAt = QStringLiteral("2026-08-01T10:00:00Z");
        o.projectRoot = root;

        const auto out = RoadmapMigrateLoad::load(store, p, o);
        EXPECT_FALSE(out.ok) << "INV-28 (" << leg.what << "): expected a refusal";
        bool sawCode = false;
        for (const auto &n : out.notes)
            if (n.code == QLatin1String("source_unplaceable")) sawCode = true;
        EXPECT_TRUE(sawCode)
            << "INV-28 (" << leg.what << "): breaks when the guard is written "
               "as \"refuse a path beginning ../\", which passes legs one and "
               "two and STORES leg three — a section that no render can place "
               "and no error reports, which is § 1's silent loss reappearing "
               "inside the fix for it";
        for (const QString &t : {QStringLiteral("project"), QStringLiteral("section"),
                                 QStringLiteral("item")}) {
            QSqlQuery q(store.db());
            ASSERT_TRUE(q.exec(QStringLiteral("SELECT COUNT(*) FROM ") + t) && q.next());
            EXPECT_EQ(q.value(0).toInt(), 0)
                << "INV-28 (" << leg.what << "): " << t.toStdString()
                << " must be empty — nothing is written";
        }
    }
}

// ANTS-3766 § 6.3 — the corpus run, over this project's ACTUAL root.
//
// DISABLED by default, following this project's existing `CorpusCalibration`
// convention: it reads a real tree outside the fixture set, so it is a
// measuring instrument rather than an assertion, and its figures are recorded
// in § 6.3 rather than enforced here. Run it with
//   ctest --test-dir build -R CorpusArchiveRun -V
// after passing ANTS_PROJECT_ROOT, or it self-skips.
//
// § 6.3 asks for three figures: the archive item count reaching the store, the
// SECOND run's Outcome (INV-9 end-to-end on real data), and the slug list
// assigned to the two real archives.
TEST(roadmap_migrate_load, DISABLED_CorpusArchiveRun) {
    const QByteArray envRoot = qgetenv("ANTS_PROJECT_ROOT");
    if (envRoot.isEmpty()) {
        GTEST_SKIP() << "set ANTS_PROJECT_ROOT to the project to migrate";
    }
    const QString root = QString::fromUtf8(envRoot);

    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    ASSERT_TRUE(disc.has_value()) << "discovery refused: " << err.toStdString();
    printf("sources: %d\n", int(disc->sources.size()));
    for (const auto &s : disc->sources)
        printf("  %-28s format=%s\n",
               QFileInfo(s.path).fileName().toUtf8().constData(),
               s.format.toUtf8().constData());

    const MigrationPlan plan =
        RoadmapMigrate::planFrom(*disc, QStringLiteral("Ants_Terminal"),
                                 QStringLiteral("ants"));

    // Figure 3 — the slug list assigned to each real archive.
    for (int i = 1; i < plan.sources.size(); ++i) {
        printf("slugs for %s:\n",
               QFileInfo(plan.sources.at(i).path).fileName().toUtf8().constData());
        for (const auto &s : plan.sections)
            if (s.sourceIndex == i)
                printf("    %s\n", s.slug.toUtf8().constData());
    }

    // Figure 1 — archive items reaching the store.
    int archiveItems = 0;
    for (const auto &it : plan.items) if (it.sourceIndex >= 1) ++archiveItems;
    printf("archive items planned: %d (of %d total)\n",
           archiveItems, int(plan.items.size()));
    for (const auto &n : plan.notes)
        printf("note: %-24s src=%d line=%d %s\n", n.code.toUtf8().constData(),
               n.sourceIndex, n.line, n.detail.left(60).toUtf8().constData());

    QTemporaryDir dbDir;
    RoadmapStore store(dbDir.path() + QStringLiteral("/roadmap.sqlite"),
                       RoadmapStore::kDefaultHistoryCapBytes,
                       RoadmapStore::Access::Bulk);
    ASSERT_TRUE(store.open(&err)) << err.toStdString();
    RoadmapMigrateLoad::Options o;
    o.changedAt = QStringLiteral("2026-08-01T10:00:00Z");
    o.projectRoot = root;

    const auto r1 = RoadmapMigrateLoad::load(store, plan, o);
    printf("run 1: ok=%d inserted=%d updated=%d unchanged=%d orphaned=%d "
           "ids=%d sections=%d elements=%d err=%s\n",
           int(r1.ok), r1.itemsInserted, r1.itemsUpdated, r1.itemsUnchanged,
           r1.itemsOrphaned, r1.idsAllocated, r1.sectionsWritten,
           r1.elementsWritten, r1.error.toUtf8().constData());
    ASSERT_TRUE(r1.ok);

    // Figure 2 — the SECOND run's Outcome, on real data.
    const auto r2 = RoadmapMigrateLoad::load(store, plan, o);
    printf("run 2: ok=%d inserted=%d updated=%d unchanged=%d orphaned=%d "
           "ids=%d sections=%d elements=%d\n",
           int(r2.ok), r2.itemsInserted, r2.itemsUpdated, r2.itemsUnchanged,
           r2.itemsOrphaned, r2.idsAllocated, r2.sectionsWritten,
           r2.elementsWritten);

    // What the archives' sections actually stored.
    QSqlQuery q(store.db());
    ASSERT_TRUE(q.exec(QStringLiteral(
        "SELECT slug, source_path FROM section WHERE source_path IS NOT NULL "
        "ORDER BY slug")));
    while (q.next())
        printf("stored: %-40s <- %s\n", q.value(0).toString().toUtf8().constData(),
               q.value(1).toString().toUtf8().constData());
}
