// Feature-conformance test for ANTS-4501 — the roadmap report.
// Contract: docs/specs/ANTS-4501-roadmap-report.md
//
// Covers the invariants the aggregate reader owns: INV-1 (coverage beside every
// bucketed figure), INV-7 (totals equal the store's own counts), INV-8
// (half-open buckets), INV-9 (a median ships the sample it came from) and
// INV-10 (the report writes nothing). INV-2/3 (backfill) and INV-4/5/6
// (stamping) belong to slices not yet built and are not asserted here — a test
// asserting them today would pass against an unwritten feature.
#include <gtest/gtest.h>

#include "roadmapclock.h"
#include "roadmapstore.h"

#include <QDate>
#include <QDir>
#include <QSqlQuery>
#include <QTemporaryDir>

namespace {

struct Fixture {
    QTemporaryDir dir;
    RoadmapStore store;
    qint64 projectId = -1;
    qint64 sectionId = -1;
    int position = 0;

    // The two-argument constructor, NOT `RoadmapStore store;` — the default
    // path resolves under XDG_DATA_HOME and is the user's REAL store.
    Fixture() : store(dir.path() + QStringLiteral("/roadmap.sqlite"),
                      RoadmapStore::kDefaultHistoryCapBytes) {
        QString err;
        EXPECT_TRUE(store.open(&err)) << err.toStdString();
        const QString root = dir.path() + QStringLiteral("/p");
        QDir().mkpath(root);
        projectId = store.registerProject(root, QStringLiteral("p"),
                                          QStringLiteral("p"), &err).value_or(-1);
        EXPECT_GT(projectId, 0) << err.toStdString();
        sectionId = store.addSection(projectId, QStringLiteral("s"),
                                     QStringLiteral("S"), 2, 0, std::nullopt,
                                     &err).value_or(-1);
        EXPECT_GT(sectionId, 0) << err.toStdString();
    }

    // `created` / `shipped` empty means NULL — the undated state every row in
    // the real store is in today, and the one INV-1 exists to keep visible.
    qint64 add(const QString &id, const QString &status, const QString &kind,
               const QString &created = {}, const QString &shipped = {}) {
        RoadmapStore::ItemWrite w;
        w.projectId = projectId;
        w.sectionId = sectionId;
        w.position  = position++;
        w.id        = id;
        w.status    = status;
        w.headline  = QStringLiteral("h");
        w.kind      = kind;
        w.source    = QStringLiteral("test");
        w.created   = created;
        w.shipped   = shipped;
        QString err;
        const auto pk = store.putItem(w, &err);
        EXPECT_TRUE(pk.has_value()) << err.toStdString();
        return pk.value_or(-1);
    }

    int scalar(const QString &sql) {
        QSqlQuery q(store.db());
        EXPECT_TRUE(q.exec(sql)) << sql.toStdString();
        EXPECT_TRUE(q.next());
        return q.value(0).toInt();
    }
};

}  // namespace

// INV-1 — every bucketed figure ships with the count of rows that could not be
// bucketed. The failure this guards is the natural SQL: `WHERE shipped IS NOT
// NULL` reports the survivors as the answer, turning a 2% sample into a
// confident total.
TEST(RoadmapReport, Inv1BucketedFiguresShipTheirUndatedCount) {
    Fixture f;
    // 3 dated closures inside this year, 7 undated ones.
    for (int i = 0; i < 3; ++i)
        f.add(QStringLiteral("ANTS-%1").arg(100 + i), QStringLiteral("shipped"),
              QStringLiteral("fix"), QStringLiteral("2026-01-05"),
              QStringLiteral("2026-03-0%1").arg(i + 1));
    for (int i = 0; i < 7; ++i)
        f.add(QStringLiteral("ANTS-%1").arg(200 + i), QStringLiteral("shipped"),
              QStringLiteral("fix"));

    QString err;
    const auto w = f.store.countInWindow(f.projectId, QDate(2026, 1, 1),
                                         QDate(2027, 1, 1), &err);
    ASSERT_TRUE(w.has_value()) << err.toStdString();
    EXPECT_EQ(w->closed, 3);

    const auto c = f.store.reportCounts(f.projectId, &err);
    ASSERT_TRUE(c.has_value()) << err.toStdString();
    // Both halves, together: the bucketed 3 is only honest beside the 7 it
    // could not see.
    EXPECT_EQ(c->shippedDated, 3);
    EXPECT_EQ(c->shippedUndated, 7);
    EXPECT_EQ(c->byStatus.value(QStringLiteral("shipped")), 10);
}

// INV-7 — the report's point-in-time totals equal the store's own row counts.
// Breaks when the report filters by something the count does not, so two verbs
// describe one roadmap differently.
TEST(RoadmapReport, Inv7TotalsEqualTheStoresOwnCounts) {
    Fixture f;
    f.add(QStringLiteral("ANTS-1"), QStringLiteral("planned"),     QStringLiteral("fix"));
    f.add(QStringLiteral("ANTS-2"), QStringLiteral("in-progress"), QStringLiteral("fix"));
    f.add(QStringLiteral("ANTS-3"), QStringLiteral("considered"),  QStringLiteral("doc"));
    f.add(QStringLiteral("ANTS-4"), QStringLiteral("shipped"),     QStringLiteral("doc"));
    f.add(QStringLiteral("ANTS-5"), QStringLiteral("dropped"),     QStringLiteral("chore"));

    QString err;
    const auto c = f.store.reportCounts(f.projectId, &err);
    ASSERT_TRUE(c.has_value()) << err.toStdString();

    EXPECT_EQ(c->items, f.scalar(QStringLiteral("SELECT COUNT(*) FROM item")));
    for (const char *s : {"planned", "in-progress", "considered", "shipped", "dropped"}) {
        EXPECT_EQ(c->byStatus.value(QLatin1String(s)),
                  f.scalar(QStringLiteral("SELECT COUNT(*) FROM item WHERE status='%1'")
                               .arg(QLatin1String(s))))
            << "by_status disagrees with a direct count for " << s;
    }
    // totals.items is the SUM of by_status including `dropped` — the identity
    // § 2.5 states, and what makes `open` reconstructible from the response.
    int sum = 0;
    for (const int n : c->byStatus)
        sum += n;
    EXPECT_EQ(sum, c->items);

    // `open` is the enumeration, never `status != 'shipped'`. With one dropped
    // row present the two forms differ, which is the whole reason § 2.5 pins it.
    const int openEnumerated = c->byStatus.value(QStringLiteral("planned"))
                             + c->byStatus.value(QStringLiteral("in-progress"))
                             + c->byStatus.value(QStringLiteral("considered"));
    EXPECT_EQ(openEnumerated, 3);
    EXPECT_NE(openEnumerated, c->items - c->byStatus.value(QStringLiteral("shipped")))
        << "a dropped item must not count as open";
}

// INV-8 — buckets are half-open, so an item falls in exactly one at each
// granularity. Breaks when both ends are inclusive, double-counting every
// boundary date. Asserted over two adjacent windows rather than two month
// buckets: the envelope emits a single `periods.month`, so the standard buckets
// cannot express the boundary at all.
TEST(RoadmapReport, Inv8BucketsAreHalfOpenSoNoItemIsCountedTwice) {
    Fixture f;
    f.add(QStringLiteral("ANTS-1"), QStringLiteral("shipped"), QStringLiteral("fix"),
          {}, QStringLiteral("2026-03-31"));   // last day of March
    f.add(QStringLiteral("ANTS-2"), QStringLiteral("shipped"), QStringLiteral("fix"),
          {}, QStringLiteral("2026-04-01"));   // first day of April

    QString err;
    const auto march = f.store.countInWindow(f.projectId, QDate(2026, 3, 1),
                                             QDate(2026, 4, 1), &err);
    ASSERT_TRUE(march.has_value()) << err.toStdString();
    const auto april = f.store.countInWindow(f.projectId, QDate(2026, 4, 1),
                                             QDate(2026, 5, 1), &err);
    ASSERT_TRUE(april.has_value()) << err.toStdString();

    EXPECT_EQ(march->closed, 1);
    EXPECT_EQ(april->closed, 1);
    // The sum is the pair, not three: 2026-04-01 belongs to April alone. An
    // inclusive upper bound would count it in both and this would read 3.
    EXPECT_EQ(march->closed + april->closed, 2);
}

// INV-9 — every median ships the sample it was computed from, and that sample
// is smaller than the population whenever a date is missing. Breaks when the
// sample reports the population, so a median over two items reads as a trend
// across ten.
TEST(RoadmapReport, Inv9MedianShipsItsSampleNotThePopulation) {
    Fixture f;
    // Three shipped rows, only two with BOTH dates known.
    f.add(QStringLiteral("ANTS-1"), QStringLiteral("shipped"), QStringLiteral("fix"),
          QStringLiteral("2026-01-01"), QStringLiteral("2026-01-11"));   // 10 days
    f.add(QStringLiteral("ANTS-2"), QStringLiteral("shipped"), QStringLiteral("fix"),
          QStringLiteral("2026-01-01"), QStringLiteral("2026-01-31"));   // 30 days
    f.add(QStringLiteral("ANTS-3"), QStringLiteral("shipped"), QStringLiteral("fix"),
          {}, QStringLiteral("2026-02-01"));                             // no created

    QString err;
    const auto ttc = f.store.timeToClose(f.projectId, &err);
    ASSERT_TRUE(ttc.has_value()) << err.toStdString();
    EXPECT_EQ(ttc->sample, 2) << "sample must be the both-dates-known population";
    EXPECT_NE(ttc->sample, 3) << "sample must not report the shipped count";
    EXPECT_EQ(ttc->medianDays, 10) << "lower median on an even sample";

    // An empty population reports sample 0 and a NULL median — distinct from a
    // genuine zero days, which is why medianDays is -1 rather than 0 here.
    Fixture empty;
    const auto none = empty.store.timeToClose(empty.projectId, &err);
    ASSERT_TRUE(none.has_value()) << err.toStdString();
    EXPECT_EQ(none->sample, 0);
    EXPECT_EQ(none->medianDays, -1);
}

// INV-9's other half — ageOfOpen measures against the seam's date, not the real
// clock, so the figure is stable whatever day the suite runs on.
TEST(RoadmapReport, Inv9AgeOfOpenMeasuresAgainstTheInjectedToday) {
    Fixture f;
    f.add(QStringLiteral("ANTS-1"), QStringLiteral("planned"), QStringLiteral("fix"),
          QStringLiteral("2026-01-01"));
    f.add(QStringLiteral("ANTS-2"), QStringLiteral("considered"), QStringLiteral("fix"),
          QStringLiteral("2026-05-01"));
    // A shipped row with a created date must NOT count as open.
    f.add(QStringLiteral("ANTS-3"), QStringLiteral("shipped"), QStringLiteral("fix"),
          QStringLiteral("2026-01-01"), QStringLiteral("2026-02-01"));

    QString err;
    const auto age = f.store.ageOfOpen(f.projectId, QDate(2026, 6, 1), &err);
    ASSERT_TRUE(age.has_value()) << err.toStdString();
    EXPECT_EQ(age->sample, 2) << "only open items with a known created date";
    EXPECT_EQ(age->oldestDays, 151);            // 2026-01-01 -> 2026-06-01
    EXPECT_EQ(age->over90, 1);                  // only the January one
}

// The § 2.2 seam itself. Without it INV-5 and INV-6 cannot be written at all:
// two writes on one real clock land on the same date, so an assertion that a
// date did or did not move holds either way.
TEST(RoadmapReport, TodaySeamOverridesAndClears) {
    const QDate real = QDate::currentDate();
    EXPECT_FALSE(RoadmapClock::todayIsOverridden());
    EXPECT_EQ(RoadmapClock::today(), real);

    RoadmapClock::setTodayForTest(QDate(2001, 2, 3));
    EXPECT_TRUE(RoadmapClock::todayIsOverridden());
    EXPECT_EQ(RoadmapClock::today(), QDate(2001, 2, 3));

    // An invalid date clears it — what a fixture's teardown passes, so one
    // test cannot leak its date into the next.
    RoadmapClock::setTodayForTest(QDate());
    EXPECT_FALSE(RoadmapClock::todayIsOverridden());
    EXPECT_EQ(RoadmapClock::today(), real);
}

// INV-10 — the report writes nothing. Asserted as row counts and the maximum
// history_id, NOT as a hash of the store file: the store opens in WAL mode, so
// a write lands in the `-wal` sidecar and leaves the main file's bytes alone
// until a checkpoint. A file hash would come back green over a report that had
// written rows.
TEST(RoadmapReport, Inv10ReportWritesNothing) {
    Fixture f;
    f.add(QStringLiteral("ANTS-1"), QStringLiteral("planned"), QStringLiteral("fix"),
          QStringLiteral("2026-01-01"));
    f.add(QStringLiteral("ANTS-2"), QStringLiteral("shipped"), QStringLiteral("fix"),
          QStringLiteral("2026-01-01"), QStringLiteral("2026-02-01"));

    const int itemsBefore   = f.scalar(QStringLiteral("SELECT COUNT(*) FROM item"));
    const int historyBefore = f.scalar(QStringLiteral("SELECT COUNT(*) FROM history"));
    const int maxHistBefore =
        f.scalar(QStringLiteral("SELECT COALESCE(MAX(history_id), 0) FROM history"));

    QString err;
    ASSERT_TRUE(f.store.reportCounts(f.projectId, &err).has_value());
    ASSERT_TRUE(f.store.countInWindow(f.projectId, QDate(2026, 1, 1),
                                      QDate(2027, 1, 1), &err).has_value());
    ASSERT_TRUE(f.store.ageOfOpen(f.projectId, QDate(2026, 6, 1), &err).has_value());
    ASSERT_TRUE(f.store.timeToClose(f.projectId, &err).has_value());

    EXPECT_EQ(f.scalar(QStringLiteral("SELECT COUNT(*) FROM item")), itemsBefore);
    EXPECT_EQ(f.scalar(QStringLiteral("SELECT COUNT(*) FROM history")), historyBefore);
    EXPECT_EQ(f.scalar(QStringLiteral("SELECT COALESCE(MAX(history_id), 0) FROM history")),
              maxHistBefore);
}

// scope:"all" is std::nullopt — every registered project summed. The store is
// machine-global, so this is the one view no single ROADMAP.md can give.
TEST(RoadmapReport, ScopeAllSumsEveryRegisteredProject) {
    Fixture f;
    f.add(QStringLiteral("ANTS-1"), QStringLiteral("planned"), QStringLiteral("fix"));

    QString err;
    const QString root2 = f.dir.path() + QStringLiteral("/q");
    QDir().mkpath(root2);
    const qint64 p2 = f.store.registerProject(root2, QStringLiteral("q"),
                                              QStringLiteral("q"), &err).value_or(-1);
    ASSERT_GT(p2, 0) << err.toStdString();
    const qint64 s2 = f.store.addSection(p2, QStringLiteral("s"), QStringLiteral("S"),
                                         2, 0, std::nullopt, &err).value_or(-1);
    ASSERT_GT(s2, 0) << err.toStdString();
    RoadmapStore::ItemWrite w;
    w.projectId = p2; w.sectionId = s2; w.position = 0;
    w.id = QStringLiteral("QQ-1"); w.status = QStringLiteral("shipped");
    w.headline = QStringLiteral("h"); w.kind = QStringLiteral("fix");
    w.source = QStringLiteral("test");
    ASSERT_TRUE(f.store.putItem(w, &err).has_value()) << err.toStdString();

    const auto one = f.store.reportCounts(f.projectId, &err);
    ASSERT_TRUE(one.has_value()) << err.toStdString();
    EXPECT_EQ(one->items, 1) << "scope:project sees only its own project";

    const auto all = f.store.reportCounts(std::nullopt, &err);
    ASSERT_TRUE(all.has_value()) << err.toStdString();
    EXPECT_EQ(all->items, 2) << "scope:all sums every registered project";
}
