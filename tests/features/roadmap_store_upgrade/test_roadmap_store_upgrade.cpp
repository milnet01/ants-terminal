// Feature-conformance test for ANTS-3781 INV-1 … INV-8 — RoadmapStore's
// schema-upgrade ladder.
// Contract: tests/features/roadmap_store_upgrade/spec.md
//
// Behavioural against a real SQLite store in a QTemporaryDir, except for the
// three source scrapes (INV-3 leg (b), INV-5, INV-6): each asserts something
// about the SHAPE of the source that no behaviour of a version-1 store can
// observe.

#include <gtest/gtest.h>

#include "roadmapstore.h"

#include <QFile>
#include <QIODevice>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QTemporaryDir>
#include <QVector>

#ifndef ANTS_SRC_DIR
#  error "ANTS_SRC_DIR compile definition required"
#endif

namespace {

using Upgrade = RoadmapStore::Upgrade;

// A real store in a throwaway directory. Never `RoadmapStore store;` — the
// default path resolves under XDG_DATA_HOME, which is the user's live roadmap.
struct Fixture {
    QTemporaryDir dir;
    RoadmapStore store;

    Fixture() : store(dir.path() + QStringLiteral("/roadmap.sqlite")) {
        QString err;
        EXPECT_TRUE(store.open(&err)) << err.toStdString();
    }
};

int userVersion(RoadmapStore &s) {
    QSqlQuery q(s.db());
    if (q.exec(QStringLiteral("PRAGMA user_version")) && q.next())
        return q.value(0).toInt();
    return -1;
}

bool hasColumn(RoadmapStore &s, const QString &table, const QString &column) {
    QSqlQuery q(s.db());
    if (!q.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table)))
        return false;
    while (q.next())
        if (q.value(1).toString() == column)
            return true;
    return false;
}

bool hasIndex(RoadmapStore &s, const QString &name) {
    QSqlQuery q(s.db());
    q.prepare(QStringLiteral(
        "SELECT count(*) FROM sqlite_master WHERE type = 'index' AND name = ?"));
    q.addBindValue(name);
    return q.exec() && q.next() && q.value(0).toInt() == 1;
}

// INV-7's common clause — every refusal names the store's version AND the
// target version. Asserted as the rendered pair rather than two loose digit
// searches: "1" and "2" turn up in almost any message by accident, so a digit
// search would pass against a message naming neither.
::testing::AssertionResult namesBothVersions(const QString &err, int from, int to) {
    const QString pair = QStringLiteral("%1 to %2").arg(from).arg(to);
    if (err.contains(pair))
        return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure()
           << "refusal names neither " << from << " nor " << to << ": "
           << err.toStdString();
}

// --- the source scrapes ------------------------------------------------------

QString srcPath(const QString &leaf) {
    return QStringLiteral(ANTS_SRC_DIR) + QStringLiteral("/") + leaf;
}

QStringList readLines(const QString &path) {
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text)) << path.toStdString();
    return QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
}

// One function definition, signature line through its closing brace.
//
// The delimiting rule, shared by INV-3 leg (b) and INV-5: from the line
// matching the signature to the next line whose FIRST CHARACTER is `}` — the
// file's style puts a closing brace at column 0 and nowhere else. A whole-file
// grep is not an option for either caller and would be GUARANTEED to fail:
// createSchema() legitimately contains BEGIN/COMMIT/ROLLBACK, and it is no
// longer the only function in the file that stamps PRAGMA user_version.
QStringList functionBody(const QStringList &lines, const QString &signature) {
    int start = -1;
    for (int i = 0; i < lines.size(); ++i) {
        if (lines.at(i).contains(signature)) {
            start = i;
            break;
        }
    }
    EXPECT_GE(start, 0) << "signature not found: " << signature.toStdString();
    if (start < 0)
        return {};
    QStringList body;
    for (int i = start; i < lines.size(); ++i) {
        body << lines.at(i);
        if (i > start && lines.at(i).startsWith(QLatin1Char('}')))
            return body;
    }
    ADD_FAILURE() << "no closing brace at column 0 after " << signature.toStdString();
    return body;
}

// INV-3 leg (b) excludes comments: the natural body comment says "runs inside
// the caller's BEGIN IMMEDIATE", and a comment-blind grep would redden a
// correct implementation.
QString withoutLineComments(const QStringList &body) {
    QStringList out;
    for (const QString &l : body) {
        const int c = l.indexOf(QStringLiteral("//"));
        out << (c >= 0 ? l.left(c) : l);
    }
    return out.join(QLatin1Char('\n'));
}

}  // namespace

// INV-1 — exactly one rung per version step in (from, to], applied in ASCENDING
// VERSION ORDER regardless of declaration order, and one stamp after the last.
// The rungs are declared descending and rung 3 references the column rung 2
// adds, so a declaration-order walk fails on SQL rather than on an assertion.
TEST(RoadmapStoreUpgrade, Inv1ClimbsAscendingAndStampsOnce) {
    Fixture f;
    ASSERT_EQ(userVersion(f.store), RoadmapStore::kSchemaVersion);

    const QVector<Upgrade> ladder = {
        Upgrade{3, {QStringLiteral("CREATE INDEX idx_up_marker ON project(up_marker)")}},
        Upgrade{2, {QStringLiteral("ALTER TABLE project ADD COLUMN up_marker TEXT")}},
    };

    QString err;
    ASSERT_TRUE(f.store.begin(&err)) << err.toStdString();
    EXPECT_TRUE(RoadmapStore::applyUpgrades(f.store.db(), 1, 3, ladder, &err))
        << err.toStdString();
    EXPECT_TRUE(hasColumn(f.store, QStringLiteral("project"), QStringLiteral("up_marker")));
    EXPECT_TRUE(hasIndex(f.store, QStringLiteral("idx_up_marker")));
    EXPECT_EQ(userVersion(f.store), 3);
    ASSERT_TRUE(f.store.commit(&err)) << err.toStdString();
    EXPECT_EQ(userVersion(f.store), 3);
}

// INV-2 leg (a) — a version in range with no rung is refused, and INV-7's
// common clause holds for that refusal.
TEST(RoadmapStoreUpgrade, Inv2MissingRungRefusedBeforeAnythingRuns) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.begin(&err)) << err.toStdString();

    err.clear();
    EXPECT_FALSE(RoadmapStore::applyUpgrades(f.store.db(), 1, 2, {}, &err));
    EXPECT_TRUE(err.contains(QStringLiteral("version 2"))) << err.toStdString();
    EXPECT_TRUE(namesBothVersions(err, 1, 2));
    EXPECT_EQ(userVersion(f.store), 1);

    ASSERT_TRUE(f.store.rollback(&err)) << err.toStdString();
}

// INV-2 leg (b) — two rungs landing on one version is the same refusal as
// none: "exactly one" is the requirement, and a ladder that offers a choice
// has no single answer to apply.
TEST(RoadmapStoreUpgrade, Inv2DuplicateRungRefusedBeforeAnythingRuns) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.begin(&err)) << err.toStdString();

    const QVector<Upgrade> ladder = {
        Upgrade{2, {QStringLiteral("ALTER TABLE project ADD COLUMN up_marker TEXT")}},
        Upgrade{2, {QStringLiteral("ALTER TABLE project ADD COLUMN up_other TEXT")}},
    };
    err.clear();
    EXPECT_FALSE(RoadmapStore::applyUpgrades(f.store.db(), 1, 2, ladder, &err));
    EXPECT_TRUE(err.contains(QStringLiteral("version 2"))) << err.toStdString();
    EXPECT_TRUE(namesBothVersions(err, 1, 2));
    EXPECT_FALSE(hasColumn(f.store, QStringLiteral("project"), QStringLiteral("up_marker")));
    EXPECT_EQ(userVersion(f.store), 1);

    ASSERT_TRUE(f.store.rollback(&err)) << err.toStdString();
}

// INV-2 leg (c) — THE leg that earns the invariant. Legs (a) and (b) are
// single-step, so a lazy per-rung lookup that validates as it goes passes both
// while breaking the "before any statement runs" clause. Here rung 2 exists and
// rung 3 does not: a lazy walk runs rung 2 first and only then discovers the
// gap, leaving a half-climbed store behind for the caller to roll back.
TEST(RoadmapStoreUpgrade, Inv2LaterMissingRungStopsTheEarlierOne) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.begin(&err)) << err.toStdString();

    const QVector<Upgrade> ladder = {
        Upgrade{2, {QStringLiteral("ALTER TABLE project ADD COLUMN up_marker TEXT")}},
    };
    err.clear();
    EXPECT_FALSE(RoadmapStore::applyUpgrades(f.store.db(), 1, 3, ladder, &err));
    EXPECT_TRUE(err.contains(QStringLiteral("version 3"))) << err.toStdString();
    EXPECT_TRUE(namesBothVersions(err, 1, 3));
    EXPECT_FALSE(hasColumn(f.store, QStringLiteral("project"), QStringLiteral("up_marker")))
        << "rung 2 ran before the walk discovered rung 3 was missing";
    EXPECT_EQ(userVersion(f.store), 1);

    ASSERT_TRUE(f.store.rollback(&err)) << err.toStdString();
}

// INV-3 leg (a) — behavioural. The climb runs entirely inside the caller's
// transaction, so a failed rung leaves the caller free to roll back and nothing
// partial survives. Also INV-7's failing-rung refusal.
TEST(RoadmapStoreUpgrade, Inv3RungFailureLeavesNothingBehind) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.begin(&err)) << err.toStdString();

    const QVector<Upgrade> ladder = {
        Upgrade{2,
                {QStringLiteral("ALTER TABLE project ADD COLUMN up_marker TEXT"),
                 QStringLiteral("THIS IS NOT SQL")}},
    };
    err.clear();
    EXPECT_FALSE(RoadmapStore::applyUpgrades(f.store.db(), 1, 2, ladder, &err));
    EXPECT_TRUE(namesBothVersions(err, 1, 2));

    ASSERT_TRUE(f.store.rollback(&err)) << err.toStdString();
    EXPECT_FALSE(hasColumn(f.store, QStringLiteral("project"), QStringLiteral("up_marker")));
    EXPECT_EQ(userVersion(f.store), 1);
}

// INV-3 leg (b) — source scrape, and NOT redundant with leg (a): leg (a) runs
// with a transaction already open, which is precisely the condition under which
// a stray BEGIN fails harmlessly and invisibly. The defect that matters shows
// up only when the precondition is broken, which no behavioural test here can
// arrange.
TEST(RoadmapStoreUpgrade, Inv3bNoTransactionControlInApplyUpgrades) {
    const QStringList body =
        functionBody(readLines(srcPath(QStringLiteral("roadmapstore.cpp"))),
                     QStringLiteral("RoadmapStore::applyUpgrades("));
    ASSERT_FALSE(body.isEmpty());
    const QString code = withoutLineComments(body);

    for (const QString &kw : {QStringLiteral("BEGIN"), QStringLiteral("COMMIT"),
                              QStringLiteral("ROLLBACK"), QStringLiteral("SAVEPOINT")}) {
        EXPECT_FALSE(code.contains(kw))
            << "applyUpgrades() runs inside the CALLER's transaction; it must not "
            << kw.toStdString();
    }
}

// INV-4 — the production ladder is complete, and carries no rung nothing will
// ever climb. GREEN AND VACUOUS at kSchemaVersion 1 by construction: both
// ranges are empty. It is a standing guard, not a red-first test — it fires on
// the first bump, which is the only moment it can.
TEST(RoadmapStoreUpgrade, Inv4ProductionLadderIsComplete) {
    const QVector<Upgrade> &ladder = RoadmapStore::upgradeLadder();

    // Rungs are INDEXED by the version they climb from and LAND one above it,
    // which is why the two ranges below differ. A single range would reject
    // every rung the first loop requires.
    for (int v = 1; v < RoadmapStore::kSchemaVersion; ++v) {
        int rungs = 0;
        for (const Upgrade &u : ladder)
            if (u.to == v + 1)
                ++rungs;
        EXPECT_EQ(rungs, 1) << "kSchemaVersion moved without exactly one rung landing on "
                            << (v + 1);
    }
    for (const Upgrade &u : ladder) {
        EXPECT_GE(u.to, 2) << "a rung landing below 2 climbs from version 0, which is the DDL's";
        EXPECT_LE(u.to, RoadmapStore::kSchemaVersion) << "a rung nothing will ever climb";
    }
}

// INV-5 — an open that upgrades does not report createdSchema(). Two legs,
// because a count alone is green against the very regression this names:
// MOVING the single assignment onto the upgrade arm leaves the count at one.
TEST(RoadmapStoreUpgrade, Inv5CreatedSchemaStaysOnTheCreationPath) {
    const QStringList lines = readLines(srcPath(QStringLiteral("roadmapstore.cpp")));
    const QRegularExpression assign(QStringLiteral("m_createdSchema\\s*=[^=]"));

    int total = 0;
    for (const QString &l : lines)
        if (assign.match(l).hasMatch())
            ++total;
    EXPECT_EQ(total, 1) << "m_createdSchema must be assigned in exactly one place";

    const QStringList body = functionBody(lines, QStringLiteral("RoadmapStore::createSchema("));
    ASSERT_FALSE(body.isEmpty());
    int stampAt = -1;
    int assignAt = -1;
    for (int i = 0; i < body.size(); ++i) {
        if (body.at(i).contains(QStringLiteral("PRAGMA user_version = %1")))
            stampAt = i;
        if (assign.match(body.at(i)).hasMatch())
            assignAt = i;
    }
    EXPECT_GE(stampAt, 0) << "createSchema()'s creation path no longer stamps user_version";
    EXPECT_GE(assignAt, 0) << "the assignment is not inside createSchema()'s body";
    EXPECT_GT(assignAt, stampAt)
        << "the assignment must follow the CREATION path's stamp, not the upgrade arm";
}

// INV-6 — the export's record version and the store's table version are
// separate constants. The grep matches the qualified RoadmapStore::kSchemaVersion
// too, and does NOT match kExportSchemaVersion (the character before its
// "SchemaVersion" is `t`), so zero is achievable and exact. Comments are in
// scope: the file's fourth mention before this change WAS a comment, and a
// comment is exactly where a re-weld would hide.
TEST(RoadmapStoreUpgrade, Inv6ExportNeverNamesTheStoresConstant) {
    for (const QString &leaf :
         {QStringLiteral("roadmapexport.cpp"), QStringLiteral("roadmapexport.h")}) {
        const QString path = srcPath(leaf);
        int hits = 0;
        for (const QString &l : readLines(path))
            hits += l.count(QStringLiteral("kSchemaVersion"));
        EXPECT_EQ(hits, 0) << path.toStdString()
                           << " names the store's table-version constant; the export "
                              "carries its own record-version constant";
    }
}

// INV-7 — the `from < 1` refusal. It has no rung to name, which is why the
// invariant's common clause is the two versions and nothing more.
TEST(RoadmapStoreUpgrade, Inv7FromBelowOneRefusesLegibly) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.begin(&err)) << err.toStdString();

    err.clear();
    EXPECT_FALSE(RoadmapStore::applyUpgrades(f.store.db(), 0, 1, {}, &err));
    EXPECT_TRUE(namesBothVersions(err, 0, 1));
    EXPECT_EQ(userVersion(f.store), 1);

    // A corrupt pragma is representable — user_version is signed — and this is
    // the arm createSchema()'s `!= 0` guard routes it to instead of the DDL.
    err.clear();
    EXPECT_FALSE(RoadmapStore::applyUpgrades(f.store.db(), -3, 1, {}, &err));
    EXPECT_TRUE(namesBothVersions(err, -3, 1));

    ASSERT_TRUE(f.store.rollback(&err)) << err.toStdString();
}

// INV-7 — the failed stamp, the one refusal the other legs cannot reach. The
// rung's LAST statement latches the connection read-only, so every rung has
// already run when the stamp is refused. Verified against SQLite before it was
// written: `PRAGMA query_only = 1; PRAGMA user_version = 2;` reports "attempt
// to write a readonly database" and leaves the version unchanged.
TEST(RoadmapStoreUpgrade, Inv7FailedStampRefusesLegibly) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.begin(&err)) << err.toStdString();

    const QVector<Upgrade> ladder = {
        Upgrade{2,
                {QStringLiteral("ALTER TABLE project ADD COLUMN up_marker TEXT"),
                 QStringLiteral("PRAGMA query_only = 1")}},
    };
    err.clear();
    EXPECT_FALSE(RoadmapStore::applyUpgrades(f.store.db(), 1, 2, ladder, &err));
    EXPECT_TRUE(namesBothVersions(err, 1, 2));
    EXPECT_EQ(userVersion(f.store), 1);

    // Unlatch before the rollback: the latch is connection-scoped and would
    // otherwise outlive this test's transaction.
    QSqlQuery(f.store.db()).exec(QStringLiteral("PRAGMA query_only = 0"));
    ASSERT_TRUE(f.store.rollback(&err)) << err.toStdString();
}

// INV-8 — a store built by the DDL and a store climbed to the same version must
// have the same shape (identical sqlite_master, derived not hardcoded).
//
// UNRUNNABLE at kSchemaVersion 1: it needs two versions to compare, and there
// is no second one. Written as a tripwire rather than a permanent skip — the
// bump is the moment the comparison becomes possible AND the moment a rung
// written from the intended shape rather than from the DDL diff would ship
// undetected, so the test that closes it must not be optional then.
TEST(RoadmapStoreUpgrade, Inv8DdlBuiltAndClimbedStoresMatch) {
    if (RoadmapStore::kSchemaVersion < 2) {
        GTEST_SKIP() << "kSchemaVersion is 1: there is no version below it to climb from, "
                        "so a DDL-built store and a climbed store cannot be compared. "
                        "ANTS-3815 makes this runnable.";
    }
    FAIL() << "kSchemaVersion has moved. ANTS-3781 INV-8 now owes this leg a body: build a "
              "store at kSchemaVersion - 1, climb it with upgradeLadder(), build a second "
              "store fresh, and compare the DERIVED sqlite_master contents (normalised for "
              "whitespace) — never a hardcoded table list.";
}
