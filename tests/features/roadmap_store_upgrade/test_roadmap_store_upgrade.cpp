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

// The "version did not move" assertions below compare against kSchemaVersion,
// not a literal: what they mean is that the fixture's store is still at the
// version it OPENED at, and the fixture opens through RoadmapStore::open(). They
// read `1` until ANTS-3815's bump, at which point six of them reddened at once
// on a number that was never the point — the same class of stale constant the
// invariants themselves are about.
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

// --- ANTS-3815 § 2.6: the frozen version-1 schema ----------------------------
//
// INV-3 and INV-4 both need a store AT VERSION 1, and a version-2 build cannot
// make one: createSchema() carries exactly one DDL and it is the current one. So
// the version-1 statements are frozen here.
//
// GENERATED, NOT TRANSCRIBED — dumped from a store built by the PRE-BUMP binary
// with exactly this query, which is the same "write it from the diff, not from
// the shape you intended" discipline the rung itself is under:
//
//     SELECT sql FROM sqlite_master WHERE sql IS NOT NULL ORDER BY rowid;
//
// Both clauses are load-bearing. `WHERE sql IS NOT NULL` drops the
// sqlite_autoindex_* rows one per UNIQUE constraint, whose `sql` is NULL and
// which need no replaying (the UNIQUE constraints recreate them); without it the
// array would carry empty statements. `ORDER BY rowid` is creation order, which
// puts every table ahead of the indexes on it — `ORDER BY name` interleaves them
// alphabetically and the replay aborts, measured, with
// `no such table: main.item` because idx_element_item sorts before item.
//
// Embedded rather than a committed .sql fixture: the bundle would need a new
// compile definition to locate the file, and this text is never edited again by
// design. Each future bump freezes one more.
constexpr const char *kSchemaV1[] = {
    R"(CREATE TABLE project (
  project_id   INTEGER PRIMARY KEY,
  root         TEXT UNIQUE,
  name         TEXT NOT NULL,
  export_slug  TEXT NOT NULL UNIQUE
                 CHECK (export_slug GLOB '[a-z0-9]*'
                    AND export_slug NOT GLOB '*[^a-z0-9-]*'),
  legend       TEXT NOT NULL DEFAULT '{}'
))",
    R"(CREATE TABLE id_prefix (
  project_id   INTEGER NOT NULL REFERENCES project(project_id),
  prefix       TEXT NOT NULL,
  high_water   INTEGER NOT NULL,
  PRIMARY KEY (project_id, prefix)
))",
    R"(CREATE TABLE section (
  section_id  INTEGER PRIMARY KEY,
  project_id  INTEGER NOT NULL REFERENCES project(project_id),
  slug        TEXT NOT NULL,
  title       TEXT NOT NULL,
  level       INTEGER NOT NULL,
  intro       TEXT,
  parent_id   INTEGER REFERENCES section(section_id),
  -- ANTS-3782 § 2.1. Which source file this section was read from,
  -- project-root-relative; NULL is the live roadmap. It is the only record of
  -- that fact -- the migration plan holds a source index and is discarded at
  -- commit -- so without it ANTS-3758 re-emits a rotated archive back into
  -- ROADMAP.md. In this DDL rather than an ALTER, and at user_version 1: no
  -- store is reachable from user-facing code yet, so there is nothing to
  -- migrate and a bump would manufacture an upgrade case in order to migrate
  -- zero stores. That freedom expires at ANTS-3758's cutover. ANTS-3781 has
  -- since built what follows -- applyUpgrades() above -- so a later column is
  -- an ALTER in a rung rather than an edit here, and it no longer touches the
  -- export goldens: the export carries its own record-version constant now.
  source_path TEXT,
  -- ANTS-3796 § 2.1. Document order among THIS PROJECT's sections -- one
  -- sequence over every section, not one per parent -- and the only record of
  -- it: the migration plan knows it while it runs (sourceIndex, firstLine) and
  -- discards it at commit, exactly as it did source_path above. Without it
  -- siblings are ordered only by the section_id surrogate, which a rebuild
  -- reassigns in (depth, slug) order, so ANTS-3758's render would re-file this
  -- project's own prose sections among its version numbers on the first
  -- recovery from backup.
  --
  -- Project-wide rather than per-parent so that parents-before-children falls
  -- out of the data (a heading precedes its subheadings in the file it was read
  -- from) instead of needing UNIQUE (parent_id, position), which SQLite would
  -- not bind anyway: it treats NULLs as distinct, so the top-level siblings
  -- that actually reorder would be unconstrained.
  --
  -- Deliberately NOT UNIQUE (project_id, position), which reads like the
  -- obvious constraint. `element` gets away with UNIQUE (section_id, position)
  -- only because clearSectionElements() lets the migration delete and rewrite
  -- the whole sequence; sections cannot be cleared (element.section_id and item
  -- filing reference them), so a re-run swapping two sections would collide
  -- mid-update with no escape -- SQLite offers DEFERRABLE INITIALLY DEFERRED
  -- for foreign keys only. Distinctness is a writer's obligation within a run's
  -- plan; sectionOrderLess()'s (position, slug) key is total regardless.
  --
  -- NOT NULL with no default, so every writer states it rather than silently
  -- taking 0. At user_version 1 for the reason source_path above records, and
  -- this is the LAST change entitled to that freedom: it expires at ANTS-3758's
  -- cutover.
  position    INTEGER NOT NULL,
  UNIQUE (project_id, slug)
))",
    R"(CREATE TABLE item (
  item_pk      INTEGER PRIMARY KEY,
  project_id   INTEGER NOT NULL REFERENCES project(project_id),
  id           TEXT NOT NULL,
  id_fold      TEXT GENERATED ALWAYS AS (lower(id)) VIRTUAL,
  id_origin    TEXT NOT NULL CHECK (id_origin IN
                 ('parsed','synthesised','quarantined')),
  status       TEXT NOT NULL CHECK (status IN
                 ('planned','in-progress','shipped','considered','dropped')),
  headline     TEXT NOT NULL,
  layman       TEXT,
  kind         TEXT NOT NULL CHECK (kind IN ('implement','fix','audit-fix','review-fix','doc','doc-fix','refactor','test','chore','release','perf','security','feature','enhancement','investigate','research','accessibility','optimize','package','marketing','ux')),
  source       TEXT NOT NULL,
  priority     INTEGER CHECK (priority IS NULL OR priority BETWEEN 1 AND 5),
  visibility   TEXT NOT NULL DEFAULT 'public'
                 CHECK (visibility IN ('public','internal')),
  milestone    TEXT,
  resolution   TEXT,
  body         TEXT,
  created      TEXT CHECK (created       IS NULL OR created       GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]'),
  last_modified TEXT CHECK (last_modified IS NULL OR last_modified GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]'),
  shipped      TEXT CHECK (shipped       IS NULL OR shipped       GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]'),
  lanes        TEXT NOT NULL DEFAULT '[]',
  evidence     TEXT NOT NULL DEFAULT '[]',
  extras       TEXT NOT NULL DEFAULT '{}',
  provenance   TEXT NOT NULL DEFAULT '{}',
  UNIQUE (project_id, id_fold)
))",
    R"(CREATE TABLE element (
  element_id  INTEGER PRIMARY KEY,
  section_id  INTEGER NOT NULL REFERENCES section(section_id),
  position    INTEGER NOT NULL,
  kind        TEXT NOT NULL CHECK (kind IN ('item','narration','table')),
  item_pk     INTEGER REFERENCES item(item_pk),
  payload     TEXT,
  UNIQUE (section_id, position),
  CHECK ((kind = 'item') = (item_pk IS NOT NULL)
     AND (kind = 'item') = (payload IS NULL))
))",
    R"(CREATE TABLE relationship (
  rel_id      INTEGER PRIMARY KEY,
  type        TEXT NOT NULL CHECK (type IN ('splits-from','blocked-by',
                'duplicate-of','supersedes','relates-to','specified-by')),
  src_pk      INTEGER NOT NULL REFERENCES item(item_pk),
  dst_pk      INTEGER REFERENCES item(item_pk),
  dst_project TEXT,
  dst_id_fold TEXT,
  dst_path    TEXT,
  CHECK ((dst_pk IS NOT NULL) + (dst_project IS NOT NULL) + (dst_path IS NOT NULL) = 1),
  CHECK ((dst_project IS NULL) = (dst_id_fold IS NULL)),
  CHECK (dst_pk IS NULL OR dst_pk <> src_pk)
))",
    R"(CREATE TABLE history (
  history_id  INTEGER PRIMARY KEY,
  item_pk     INTEGER NOT NULL REFERENCES item(item_pk),
  changed_at  TEXT NOT NULL
                CHECK (changed_at GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]T[0-9][0-9]:[0-9][0-9]:[0-9][0-9]Z'),
  seq         INTEGER NOT NULL,
  field       TEXT NOT NULL,
  old_value   TEXT,
  new_value   TEXT,
  UNIQUE (item_pk, changed_at, seq)
))",
    R"(CREATE TABLE feedback_ref (
  item_pk     INTEGER NOT NULL REFERENCES item(item_pk),
  file        TEXT NOT NULL,
  PRIMARY KEY (item_pk, file)
))",
    R"(CREATE TABLE citation (
  citation_id INTEGER PRIMARY KEY,
  project_id  INTEGER NOT NULL REFERENCES project(project_id),
  item_pk     INTEGER REFERENCES item(item_pk),
  doc_path    TEXT,
  target_file TEXT NOT NULL,
  symbol      TEXT NOT NULL DEFAULT '',
  CHECK ((item_pk IS NULL) != (doc_path IS NULL))
))",
    R"(CREATE UNIQUE INDEX rel_item_uq  ON relationship(type, src_pk, dst_pk) WHERE dst_pk IS NOT NULL)",
    R"(CREATE UNIQUE INDEX rel_xproj_uq ON relationship(type, src_pk, dst_project, dst_id_fold) WHERE dst_project IS NOT NULL)",
    R"(CREATE UNIQUE INDEX rel_doc_uq   ON relationship(type, src_pk, dst_path) WHERE dst_path IS NOT NULL)",
    R"(CREATE UNIQUE INDEX cite_item_uq ON citation(item_pk, target_file, symbol) WHERE item_pk IS NOT NULL)",
    R"(CREATE UNIQUE INDEX cite_doc_uq  ON citation(project_id, doc_path, target_file, symbol) WHERE doc_path IS NOT NULL)",
    R"(CREATE UNIQUE INDEX elem_item_uq ON element(item_pk) WHERE kind = 'item')",
    R"(CREATE INDEX idx_section_parent ON section(parent_id))",
    R"(CREATE INDEX idx_element_item   ON element(item_pk))",
    R"(CREATE INDEX idx_rel_src        ON relationship(src_pk))",
    R"(CREATE INDEX idx_rel_dst        ON relationship(dst_pk))",
    R"(CREATE INDEX idx_citation_proj  ON citation(project_id))",
};

// Writes the frozen schema into `path` through a RAW connection and stamps
// user_version = 1.
//
// NEVER through RoadmapStore::open(): a version-2 build's open() would create
// the version-2 shape, which is the whole thing this fixture exists to avoid.
// The prohibition is on the SEEDING only — reopening the seeded file through
// open() afterwards is not an exception to it, it IS the climb under test.
bool seedVersion1Store(const QString &path, const QString &connName) {
    bool ok = true;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        db.setDatabaseName(path);
        if (!db.open()) {
            ADD_FAILURE() << "seed open: " << db.lastError().text().toStdString();
            ok = false;
        } else {
            QSqlQuery q(db);
            for (const char *stmt : kSchemaV1) {
                if (!q.exec(QString::fromUtf8(stmt))) {
                    ADD_FAILURE() << "seed statement failed: "
                                  << q.lastError().text().toStdString();
                    ok = false;
                    break;
                }
            }
            if (ok && !q.exec(QStringLiteral("PRAGMA user_version = 1"))) {
                ADD_FAILURE() << "seed stamp: " << q.lastError().text().toStdString();
                ok = false;
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connName);
    return ok;
}

// ANTS-3815 § 2.5's normalisation, and the ONLY one this comparison permits.
//
// SQLite does not re-render a table's stored SQL after ALTER TABLE ADD COLUMN —
// it splices the new column in ahead of the closing paren and moves the comma to
// the START of the appended text — so a DDL-built store and a climbed one differ
// by a space before a comma. Measured on SQLite 3.53.2: byte comparison says
// unequal, collapsing whitespace runs says unequal, deleting ALL whitespace says
// equal but mangles any literal containing a space. Hence: collapse runs, then
// delete spaces adjacent to `,` `(` `)`, then trim.
//
// Residual limitation, stated because it is real: the rule still normalises away
// a difference inside a literal holding one of those characters beside a space
// (`DEFAULT 'a, b'` and `DEFAULT 'a,b'` compare equal). No such literal exists in
// this schema — every default is '', '{}', '[]' or 'public' — and introducing one
// is the trigger to revisit this rule rather than to widen it quietly.
QString normaliseDdl(const QString &sql) {
    static const QRegularExpression runs(QStringLiteral("\\s+"));
    static const QRegularExpression around(QStringLiteral(" *([,()]) *"));
    QString s = sql;
    s.replace(runs, QStringLiteral(" "));
    s.replace(around, QStringLiteral("\\1"));
    return s.trimmed();
}

// The weaker rule ANTS-3781 INV-8 first stated, kept ONLY as INV-3's negative
// control: substituting it must redden.
QString collapseWhitespaceOnly(const QString &sql) {
    static const QRegularExpression runs(QStringLiteral("\\s+"));
    QString s = sql;
    s.replace(runs, QStringLiteral(" "));
    return s.trimmed();
}

// Every statement sqlite_master holds, DERIVED — never a hardcoded table list,
// so the assertion survives the schema growing. Sorted, because the comparison
// is about the schema's CONTENTS and not the order two builds happened to create
// them in; a rung that rebuilt a table would still be caught, by the UNIQUE and
// foreign-key clauses that vanish from its CREATE TABLE text.
QStringList schemaStatements(QSqlDatabase &db, QString (*norm)(const QString &)) {
    QStringList out;
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral(
            "SELECT sql FROM sqlite_master WHERE sql IS NOT NULL ORDER BY rowid"))) {
        ADD_FAILURE() << "sqlite_master read: " << q.lastError().text().toStdString();
        return out;
    }
    while (q.next())
        out << norm(q.value(0).toString());
    out.sort();
    return out;
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
    EXPECT_EQ(userVersion(f.store), RoadmapStore::kSchemaVersion);

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
    EXPECT_EQ(userVersion(f.store), RoadmapStore::kSchemaVersion);

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
    EXPECT_EQ(userVersion(f.store), RoadmapStore::kSchemaVersion);

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
    EXPECT_EQ(userVersion(f.store), RoadmapStore::kSchemaVersion);
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
    EXPECT_EQ(userVersion(f.store), RoadmapStore::kSchemaVersion);

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
    EXPECT_EQ(userVersion(f.store), RoadmapStore::kSchemaVersion);

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
// The name is ANTS-3781's and is kept: it is a handle cited from that spec's
// INV-8, which ANTS-3815 § 3 renumbers locally as its own INV-3 without renaming
// the case. Runnable since that bump supplied the first rung.
TEST(RoadmapStoreUpgrade, Inv8DdlBuiltAndClimbedStoresMatch) {
    ASSERT_GE(RoadmapStore::kSchemaVersion, 2)
        << "this leg needs two versions to compare";

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    // (1) The DDL-built store — createSchema()'s current CREATE TABLE text.
    const QString ddlPath = dir.filePath(QStringLiteral("ddl.sqlite"));
    QStringList ddlBuilt, ddlBuiltWeak;
    {
        RoadmapStore fresh(ddlPath);
        QString err;
        ASSERT_TRUE(fresh.open(&err)) << err.toStdString();
        ASSERT_EQ(userVersion(fresh), RoadmapStore::kSchemaVersion);
        ddlBuilt     = schemaStatements(fresh.db(), &normaliseDdl);
        ddlBuiltWeak = schemaStatements(fresh.db(), &collapseWhitespaceOnly);
    }

    // (2) The climbed store — seeded at version 1, then opened by this build,
    // which routes it through applyUpgrades() and the production ladder.
    const QString climbPath = dir.filePath(QStringLiteral("climb.sqlite"));
    ASSERT_TRUE(seedVersion1Store(climbPath, QStringLiteral("ants3815_inv3_seed")));
    QStringList climbed, climbedWeak;
    {
        RoadmapStore store(climbPath);
        QString err;
        ASSERT_TRUE(store.open(&err)) << err.toStdString();
        ASSERT_EQ(userVersion(store), RoadmapStore::kSchemaVersion);
        climbed     = schemaStatements(store.db(), &normaliseDdl);
        climbedWeak = schemaStatements(store.db(), &collapseWhitespaceOnly);
    }

    ASSERT_FALSE(ddlBuilt.isEmpty());
    // Compared as joined text, not as two QStringLists: gtest prints a container
    // of QStrings as a wall of `2-byte object <43-00>` and the actual difference
    // is unreadable, which on the first run cost a round-trip to find a
    // one-comment mismatch.
    EXPECT_EQ(ddlBuilt.join(QLatin1Char('\n')).toStdString(),
              climbed.join(QLatin1Char('\n')).toStdString())
        << "a rung written from the intended shape rather than from the DDL diff "
           "ships two different databases both labelled version "
        << RoadmapStore::kSchemaVersion;

    // The negative leg: § 2.5's rule is not interchangeable with the weaker one
    // ANTS-3781 INV-8 first wrote, and substituting it must REDDEN rather than
    // quietly pass. Pinned to how SQLite 3.53.x renders an ALTER'd table — it
    // splices the column ahead of the closing paren and leaves a space before the
    // comma. A driver that ever re-rendered the stored SQL would make two correct
    // schemas compare equal here and red this leg; the response then is to DROP
    // this leg, never to weaken the positive comparison it guards.
    EXPECT_NE(ddlBuiltWeak.join(QLatin1Char('\n')).toStdString(),
              climbedWeak.join(QLatin1Char('\n')).toStdString())
        << "collapsing whitespace runs alone made the two compare equal, so it "
           "could be substituted for § 2.5's rule without any test noticing";
}

// ANTS-3815 INV-4 — a version-1 store keeps its DATA across the climb, and the
// climb is not mistaken for a creation.
//
// The rung mutation this catches and INV-3 cannot: a rung with no DEFAULT. SQLite
// refuses ADD COLUMN … NOT NULL with no default on a table that HAS ROWS and
// accepts it on an empty one, so the project row inserted below is what makes the
// case reachable at all — against an empty store a defaultless rung passes.
TEST(RoadmapStoreUpgrade, Ants3815Inv4ClimbPreservesDataAndIsNotACreation) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("v1.sqlite"));
    ASSERT_TRUE(seedVersion1Store(path, QStringLiteral("ants3815_inv4_seed")));

    // A project row, written raw because registerProject() belongs to the build
    // that cannot open this file yet.
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                    QStringLiteral("ants3815_inv4_row"));
        db.setDatabaseName(path);
        ASSERT_TRUE(db.open()) << db.lastError().text().toStdString();
        QSqlQuery q(db);
        ASSERT_TRUE(q.exec(QStringLiteral(
            "INSERT INTO project (root, name, export_slug, legend) "
            "VALUES ('/somewhere/proj', 'Proj', 'proj', '{\"a\":1}')")))
            << q.lastError().text().toStdString();
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("ants3815_inv4_row"));

    RoadmapStore store(path);
    QString err;
    ASSERT_TRUE(store.open(&err)) << err.toStdString();

    EXPECT_EQ(userVersion(store), RoadmapStore::kSchemaVersion);
    EXPECT_FALSE(store.createdSchema())
        << "an upgraded store's tables were made by an earlier binary (ANTS-3781 INV-5)";

    const auto row = store.readProject(1, &err);
    ASSERT_TRUE(row.has_value()) << err.toStdString();
    EXPECT_EQ(row->root.toStdString(), std::string("/somewhere/proj"));
    EXPECT_EQ(row->name.toStdString(), std::string("Proj"));
    EXPECT_EQ(row->exportSlug.toStdString(), std::string("proj"));
    EXPECT_EQ(row->legendText.toStdString(), std::string("{\"a\":1}"));
    EXPECT_TRUE(row->sourceFormat.isEmpty())
        << "a row written before the column existed reads back as '' — not "
           "recorded, and § 2.4's version-1 dispatch path";
}
