// Feature-conformance test for ANTS-3756 INV-15 / INV-16 — two writers against
// one roadmap store. Contract: tests/features/roadmap_store_concurrency/spec.md
//
// Behavioural, and for INV-15 genuinely multi-process: SQLite's write lock
// distinguishes connections, but the creation race the invariant describes is
// two Ants instances, so it is tested with fork(2) rather than two connections.
// The fork lives inside this bundle — ANTS-1217 consolidated 141 standalone
// binaries into seven, and a new target here would reverse that.

#include <gtest/gtest.h>

#include "roadmapstore.h"

#include <QDir>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QTemporaryDir>

#include <sys/wait.h>
#include <unistd.h>

namespace {

// What a forked opener reports. Three outcomes, not two: "both opened" is not
// the invariant — exactly one of them must have CREATED.
constexpr int kCreated = 10;     // ran the DDL and set user_version
constexpr int kJoined = 11;      // saw user_version = 1, created nothing
constexpr int kOpenFailed = 12;

QString tableList(RoadmapStore &s) {
    QStringList names;
    QSqlQuery q(s.db());
    if (q.exec(QStringLiteral(
            "SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name")))
        while (q.next())
            names << q.value(0).toString();
    return names.join(QLatin1Char(','));
}

int userVersion(RoadmapStore &s) {
    QSqlQuery q(s.db());
    if (q.exec(QStringLiteral("PRAGMA user_version")) && q.next())
        return q.value(0).toInt();
    return -1;
}

int busyTimeout(RoadmapStore &s) {
    QSqlQuery q(s.db());
    if (q.exec(QStringLiteral("PRAGMA busy_timeout")) && q.next())
        return q.value(0).toInt();
    return -1;
}

}  // namespace

// INV-15 — two processes opening a store that does not exist produce exactly
// one schema: one creates it, the other observes user_version = 1 inside its
// own BEGIN IMMEDIATE and creates nothing.
TEST(RoadmapStoreConcurrency, Inv15ForkedOpenersProduceExactlyOneSchema) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid()) << dir.errorString().toStdString();
    const QString path = dir.path() + QStringLiteral("/roadmap.sqlite");

    // Both children block on this pipe and are released by one write, so they
    // contend for the store rather than running one after the other.
    int gate[2] = {-1, -1};
    ASSERT_EQ(::pipe(gate), 0);

    pid_t kids[2] = {-1, -1};
    for (pid_t &kid : kids) {
        kid = ::fork();
        ASSERT_NE(kid, -1) << "fork failed";
        if (kid == 0) {
            ::close(gate[1]);
            char go = 0;
            int code = kOpenFailed;
            if (::read(gate[0], &go, 1) == 1) {
                RoadmapStore store(path);
                QString err;
                if (store.open(&err))
                    code = store.createdSchema() ? kCreated : kJoined;
                else
                    // The parent only sees an exit code; without this the
                    // reason an opener lost is unrecoverable.
                    ::fprintf(stderr, "opener failed: %s\n", qPrintable(err));
            }  // scope end closes the connection before the process exits
            // _exit, never exit(): QTemporaryDir's destructor would delete the
            // store the parent is about to assert on.
            ::_exit(code);
        }
    }

    ::close(gate[0]);
    ASSERT_EQ(::write(gate[1], "gg", 2), 2);
    ::close(gate[1]);

    int created = 0, joined = 0, failed = 0;
    for (pid_t kid : kids) {
        int status = 0;
        ASSERT_EQ(::waitpid(kid, &status, 0), kid);
        ASSERT_TRUE(WIFEXITED(status)) << "an opener died on a signal";
        switch (WEXITSTATUS(status)) {
        case kCreated: ++created; break;
        case kJoined: ++joined; break;
        default: ++failed; break;
        }
    }

    EXPECT_EQ(failed, 0) << "both openers must succeed — the loser waits out the "
                            "write lock, it does not error";
    EXPECT_EQ(created, 1) << "exactly one opener may create the schema; two means "
                             "creation gated on something that succeeds for both";
    EXPECT_EQ(joined, 1) << "the other must observe user_version = 1 and create nothing";

    // And the race left the store a single process would have left.
    QString err;
    RoadmapStore raced(path);
    ASSERT_TRUE(raced.open(&err)) << err.toStdString();
    EXPECT_FALSE(raced.createdSchema()) << "the schema was already there";
    EXPECT_EQ(userVersion(raced), RoadmapStore::kSchemaVersion);

    QTemporaryDir soloDir;
    ASSERT_TRUE(soloDir.isValid()) << soloDir.errorString().toStdString();
    RoadmapStore solo(soloDir.path() + QStringLiteral("/roadmap.sqlite"));
    ASSERT_TRUE(solo.open(&err)) << err.toStdString();
    ASSERT_TRUE(solo.createdSchema());

    // Derived, not hardcoded: the assertion stays true as the schema grows.
    EXPECT_EQ(tableList(raced).toStdString(), tableList(solo).toStdString())
        << "the raced store must hold exactly the schema one process creates";
}

// INV-16 leg 1 — the deadline in force on a store connection is 5000 ms, the
// value § 2.5 pins to ConfigWriteLock's. Deliberately the EFFECTIVE deadline
// and not "our pragma ran": Qt's QSQLITE plugin sets 5000 ms of its own accord,
// so dropping the pragma leaves this green (measured) while drifting the
// constant reddens it.
TEST(RoadmapStoreConcurrency, Inv16BusyTimeoutDeadlineIsApplied) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid()) << dir.errorString().toStdString();
    RoadmapStore store(dir.path() + QStringLiteral("/roadmap.sqlite"));
    QString err;
    ASSERT_TRUE(store.open(&err)) << err.toStdString();

    EXPECT_EQ(busyTimeout(store), 5000)
        << "every connection must carry the 5000 ms deadline that matches "
           "ConfigWriteLock's — SQLite's own default is 0, an immediate "
           "SQLITE_BUSY, so nothing about this is free";
}

// INV-16 leg 2 — a write that cannot take the lock within the deadline FAILS
// AND REPORTS, and leaves nothing behind. Never retried silently, never
// dropped: a lost roadmap write is invisible to the user.
TEST(RoadmapStoreConcurrency, Inv16BlockedWriteFailsAndWritesNothing) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid()) << dir.errorString().toStdString();
    const QString path = dir.path() + QStringLiteral("/roadmap.sqlite");
    QString err;

    RoadmapStore holder(path);
    ASSERT_TRUE(holder.open(&err)) << err.toStdString();

    const QString root = dir.path() + QStringLiteral("/proj");
    QDir().mkpath(root);
    const auto project =
        holder.registerProject(root, QStringLiteral("proj"), QStringLiteral("proj"), &err);
    ASSERT_TRUE(project.has_value()) << err.toStdString();
    const auto section = holder.addSection(*project, QStringLiteral(""), QStringLiteral(""), 0,
                                           std::nullopt, &err);
    ASSERT_TRUE(section.has_value()) << err.toStdString();

    RoadmapStore blocked(path);
    ASSERT_TRUE(blocked.open(&err)) << err.toStdString();

    // The store's deadline is 5000 ms (asserted in leg 1). Shortening it on
    // THIS connection exercises the policy — fail, report, write nothing —
    // without making the suite wait five seconds to re-assert the constant.
    {
        QSqlQuery t(blocked.db());
        ASSERT_TRUE(t.exec(QStringLiteral("PRAGMA busy_timeout = 100")));
    }

    // A real write, not a bare BEGIN: the lock must be held, not merely reserved.
    {
        QSqlQuery b(holder.db());
        ASSERT_TRUE(b.exec(QStringLiteral("BEGIN IMMEDIATE")))
            << b.lastError().text().toStdString();
        QSqlQuery w(holder.db());
        ASSERT_TRUE(w.exec(QStringLiteral("UPDATE project SET name = 'held'")))
            << w.lastError().text().toStdString();
    }

    RoadmapStore::ItemWrite item;
    item.projectId = *project;
    item.sectionId = *section;
    item.id = QStringLiteral("Sh-1");
    item.status = QStringLiteral("planned");
    item.headline = QStringLiteral("h");
    item.kind = QStringLiteral("implement");
    item.source = QStringLiteral("test");

    err.clear();
    const auto pk = blocked.putItem(item, &err);
    EXPECT_FALSE(pk.has_value())
        << "a write that cannot take the lock must fail, not appear to succeed";
    EXPECT_FALSE(err.isEmpty()) << "...and it must REPORT: a silently dropped write is "
                                   "indistinguishable from one that never happened";

    {
        QSqlQuery r(holder.db());
        ASSERT_TRUE(r.exec(QStringLiteral("ROLLBACK")));
    }

    QSqlQuery c(holder.db());
    ASSERT_TRUE(c.exec(QStringLiteral("SELECT count(*) FROM item")));
    ASSERT_TRUE(c.next());
    EXPECT_EQ(c.value(0).toInt(), 0) << "a refused write may leave no row behind";
}
