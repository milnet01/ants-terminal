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
#include <QFileInfo>
#include <QSqlQuery>
#include <QTemporaryDir>

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
    p.sourcePath = QStringLiteral("/nowhere/ROADMAP.md");
    p.format = QStringLiteral("ants-v1");
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
