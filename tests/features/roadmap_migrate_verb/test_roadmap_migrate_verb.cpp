// Feature-conformance test for ANTS-3855 — the `roadmap_migrate` verb.
// Contract: tests/features/roadmap_migrate_verb/spec.md, which points at
// docs/specs/ANTS-3855-roadmap-migrate-verb.md.
//
// Every behavioural invariant drives RoadmapMigrateVerb::run(storePath, req)
// directly, with `storePath` inside a QTemporaryDir. The registered handler is
// not driven by any test — its whole remaining job is to resolve
// RoadmapStore::defaultPath(), which is the developer's REAL store, and INV-2's
// source-grep leg covers that one line instead.
//
// INV-1 and INV-2(b) are source greps and run in-process against ANTS_SRC_DIR:
// shelling out to `rg` would make them pass vacuously wherever it is absent,
// and it is not a declared build or test dependency.

#include <gtest/gtest.h>

#include "roadmapmigrateverb.h"
#include "roadmapsource.h"
#include "roadmapstore.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <sys/stat.h>

#include <memory>

#ifndef ANTS_SRC_DIR
#  error "ANTS_SRC_DIR compile definition required"
#endif

namespace {

// ------------------------------------------------------------ source greps --

// Every .h/.cpp under the source tree.
QStringList sourceFiles() {
    QStringList out;
    QDirIterator it(QStringLiteral(ANTS_SRC_DIR),
                    {QStringLiteral("*.h"), QStringLiteral("*.cpp")},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        out.append(it.next());
    out.sort();
    return out;
}

// Which files contain a CODE line matching `rx`.
//
// A line whose first NON-WHITESPACE characters are `//` is skipped. Not "the
// line begins with //": the two surviving mentions of these entry points are
// INDENTED member comments in src/roadmapstore.h and src/roadmapmigrate.h, so a
// column-0 rule would red against correct code. The spec's own `grep -v ': *//'`
// tolerates the indent for the same reason.
QStringList filesWithCodeMatch(const QRegularExpression &rx) {
    QStringList hits;
    const QStringList files = sourceFiles();
    for (const QString &path : files) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            if (line.trimmed().startsWith(QStringLiteral("//")))
                continue;
            if (rx.match(line).hasMatch()) {
                hits.append(QFileInfo(path).fileName());
                break;
            }
        }
    }
    return hits;
}

QRegularExpression callShape(const QString &qualifiedName) {
    return QRegularExpression(QRegularExpression::escape(qualifiedName)
                              + QStringLiteral("\\s*\\("));
}

// ---------------------------------------------------------------- fixtures --

bool writeFile(const QString &path, const QByteArray &text) {
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return f.write(text) == text.size();
}

QString readAll(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(f.readAll());
}

// § 2.3's precondition: run() takes an ALREADY-canonical root, because that is
// the form registerProject() stores and readProjectByRoot() looks up by. A
// QTemporaryDir under a symlinked /tmp would otherwise make every re-run look
// like a new project and INV-7 would fail for a reason that is not the rule.
QString makeProjectRoot(const QTemporaryDir &dir, const QString &leaf,
                        const QByteArray &roadmap) {
    const QString root = dir.filePath(leaf);
    if (!writeFile(root + QStringLiteral("/ROADMAP.md"), roadmap))
        return QString();
    return QFileInfo(root).canonicalFilePath();
}

// A minimal well-formed ants-v1 roadmap. `headline` is a parameter so INV-5 can
// edit one field between two runs.
QByteArray demoRoadmap(const QString &headline = QStringLiteral("An open item.")) {
    QByteArray md =
        "<!-- ants-roadmap-format: 1 -->\n"
        "\n"
        "# Demo — Roadmap\n"
        "\n"
        "## Work\n"
        "\n"
        "- \xF0\x9F\x93\x8B [DEMO-0001] **";
    md += headline.toUtf8();
    md +=
        "**\n"
        "  Layman: A thing.\n"
        "  Kind: implement.\n"
        "  Source: test.\n";
    return md;
}

RoadmapMigrateVerb::Request request(const QString &root,
                                    const QString &slug = QStringLiteral("demo"),
                                    const QString &name = QStringLiteral("Demo")) {
    RoadmapMigrateVerb::Request r;
    r.projectRoot = root;
    r.projectName = name;
    r.exportSlug  = slug;
    r.changedAt   = QStringLiteral("2026-08-06T10:00:00Z");
    return r;
}

// ------------------------------------------------------------- inspection ---

// NEVER default-construct RoadmapStore: it resolves defaultPath() — the
// developer's REAL store under XDG_DATA_HOME — so every case here would read
// and write into it.
std::unique_ptr<RoadmapStore> openStore(const QString &path,
                                        RoadmapStore::Access access) {
    auto store = std::make_unique<RoadmapStore>(
        path, RoadmapStore::kDefaultHistoryCapBytes, access);
    QString err;
    if (!store->open(&err)) {
        ADD_FAILURE() << "store open: " << err.toStdString();
        return nullptr;
    }
    return store;
}

const QStringList &rowTables() {
    static const QStringList kTables = {
        QStringLiteral("project"), QStringLiteral("section"),
        QStringLiteral("item"), QStringLiteral("element"),
        QStringLiteral("history"),
    };
    return kTables;
}

// run() owns its connection for the duration of the call and hands back only a
// QJsonObject, so a clause that inspects rows opens its OWN Interactive store at
// the same path AFTER run() returns.
QList<int> rowCounts(const QString &storePath) {
    QList<int> counts;
    auto store = openStore(storePath, RoadmapStore::Access::Interactive);
    if (!store)
        return counts;
    for (const QString &table : rowTables()) {
        QSqlQuery q(store->db());
        if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM %1").arg(table)) || !q.next()) {
            ADD_FAILURE() << "count " << table.toStdString() << ": "
                          << q.lastError().text().toStdString();
            return {};
        }
        counts.append(q.value(0).toInt());
    }
    return counts;
}

QString describeCounts(const QList<int> &counts) {
    QStringList parts;
    for (int i = 0; i < counts.size() && i < rowTables().size(); ++i)
        parts.append(QStringLiteral("%1=%2").arg(rowTables().at(i)).arg(counts.at(i)));
    return parts.join(QStringLiteral(" "));
}

// The count fields the envelope carries, in a stable order, for INV-3's dry-run
// vs. real-run comparison.
QStringList countFields() {
    return {
        QStringLiteral("items_inserted"),  QStringLiteral("items_updated"),
        QStringLiteral("items_unchanged"), QStringLiteral("items_orphaned"),
        QStringLiteral("ids_allocated"),   QStringLiteral("sections_written"),
        QStringLiteral("elements_written"), QStringLiteral("history_rows"),
    };
}

// RAII, per the project's test-env convention. Without it INV-9 passes
// vacuously on any machine or CI runner already running `umask 077`: the file
// would be 0600 whether or not setOwnerOnlyPerms() ran, and the rule under test
// would not be the rule making the fixture pass.
class ScopedUmask {
public:
    explicit ScopedUmask(mode_t mask) : m_previous(::umask(mask)) {}
    ~ScopedUmask() { ::umask(m_previous); }
    ScopedUmask(const ScopedUmask &) = delete;
    ScopedUmask &operator=(const ScopedUmask &) = delete;

private:
    mode_t m_previous;
};

}  // namespace

// ---------------------------------------------------------------- INV-1 -----
//
// A non-test translation unit calls all three migration entry points, and only
// the verb's TU does. Until ANTS-3855 the whole migration engine was shipped and
// unreachable: every invoker lived under tests/features/.

TEST(RoadmapMigrateVerb, Inv1SingleProductionCallSite) {
    const QStringList entryPoints = {
        QStringLiteral("RoadmapMigrate::findRoadmaps"),
        QStringLiteral("RoadmapMigrate::planFrom"),
        QStringLiteral("RoadmapMigrateLoad::load"),
    };
    // The SEAM's TU, not the handler's. The two are separate objects on
    // purpose (see roadmapmigrateverb.h): a static archive is pulled in at
    // object granularity, so a seam sharing an object with
    // RemoteControl::cmdRoadmapMigrate could not be linked into test_core at
    // all — which is where every behavioural leg below runs.
    const QString verbTu = QStringLiteral("roadmapmigrateverb.cpp");

    for (const QString &entry : entryPoints) {
        const QStringList hits = filesWithCodeMatch(callShape(entry));
        EXPECT_EQ(hits, QStringList{verbTu})
            << entry.toStdString() << " must be called from " << verbTu.toStdString()
            << " and from nowhere else under src/; got ["
            << hits.join(QStringLiteral(", ")).toStdString() << "]";
    }
}

// ------------------------------------------------------------- INV-2(b) -----
//
// run() migrates on a connection it opened itself and cannot reach the
// process-owned Access::Interactive one. The signature is what enforces that,
// so the grep asserts the signature — and that no code line in either file of
// the verb reaches for RemoteControl's shared store. § 2.2's prose says exactly
// that in words, which is the kind of rationale the TU would carry as a comment,
// so the comment-skipping rule above is load-bearing here too.

TEST(RoadmapMigrateVerb, Inv2bOwnBulkConnectionBySignature) {
    const QString dir = QStringLiteral(ANTS_SRC_DIR);
    const QString headerPath  = dir + QStringLiteral("/roadmapmigrateverb.h");
    const QString seamPath    = dir + QStringLiteral("/roadmapmigrateverb.cpp");
    const QString handlerPath = dir + QStringLiteral("/remotecontrol_roadmap_migrate.cpp");

    const auto codeLines = [](const QString &path) -> QStringList {
        QStringList out;
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            ADD_FAILURE() << "cannot read " << path.toStdString();
            return out;
        }
        const QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            if (line.trimmed().startsWith(QStringLiteral("//")))
                continue;
            out.append(line);
        }
        return out;
    };

    const QStringList header  = codeLines(headerPath);
    const QStringList seam    = codeLines(seamPath);
    const QStringList handler = codeLines(handlerPath);
    ASSERT_FALSE(header.isEmpty());
    ASSERT_FALSE(seam.isEmpty());
    ASSERT_FALSE(handler.isEmpty());

    for (const QString &line : header + seam + handler) {
        EXPECT_FALSE(line.contains(QStringLiteral("roadmapStoreOrNull")))
            << "the verb must open its own Access::Bulk store, never the "
               "process-owned Interactive one: " << line.trimmed().toStdString();
    }

    // The declared signature takes a path, not an already-open store. A
    // `RoadmapStore &` parameter would let the process-owned connection in
    // through the front door and make the rule above unenforceable.
    const QString headerText = header.join(QLatin1Char('\n'));
    EXPECT_TRUE(headerText.contains(
        QRegularExpression(QStringLiteral(
            R"(QJsonObject\s+run\s*\(\s*const\s+QString\s*&\s*storePath\s*,)"))))
        << "run() must declare a `const QString &storePath` first parameter";
    EXPECT_FALSE(headerText.contains(QStringLiteral("RoadmapStore &")))
        << "run() must not take an open store";
}

// ------------------------------------------------------------- INV-2(a) -----
//
// ok:true alone would also pass for a *Bulk* process-owned connection, which is
// why (b) exists — but it does separate the two live possibilities that matter
// here: RoadmapMigrateLoad::load() refuses an Interactive connection outright
// (ANTS-3765 INV-12), so a verb that reused the process-owned store could not
// return ok:true at all.

TEST(RoadmapMigrateVerb, Inv2aMigratesOnItsOwnBulkStore) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = makeProjectRoot(dir, QStringLiteral("proj"), demoRoadmap());
    ASSERT_FALSE(root.isEmpty());

    const QJsonObject env =
        RoadmapMigrateVerb::run(dir.filePath(QStringLiteral("store.sqlite")),
                                request(root));
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool())
        << env.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_GT(env.value(QStringLiteral("project_id")).toInt(), 0);
    EXPECT_EQ(env.value(QStringLiteral("items_inserted")).toInt(), 1);
}

// ---------------------------------------------------------------- INV-3 -----

TEST(RoadmapMigrateVerb, Inv3DryRunCommitsNothingAndPreviewsTruly) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = makeProjectRoot(dir, QStringLiteral("proj"), demoRoadmap());
    ASSERT_FALSE(root.isEmpty());
    const QString storePath = dir.filePath(QStringLiteral("store.sqlite"));

    auto req = request(root);
    req.dryRun = true;
    const QJsonObject dry = RoadmapMigrateVerb::run(storePath, req);
    ASSERT_TRUE(dry.value(QStringLiteral("ok")).toBool())
        << dry.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_TRUE(dry.value(QStringLiteral("dry_run")).toBool());

    // § 2.3.1 — the dry run legitimately CREATES the store, because its counts
    // are a diff against what the store already holds and a throwaway store
    // would report every item as an insert on an already-migrated project. What
    // it may create is an empty SCHEMA, which is why this asserts rows.
    const QList<int> afterDry = rowCounts(storePath);
    ASSERT_EQ(afterDry.size(), rowTables().size());
    for (int i = 0; i < afterDry.size(); ++i) {
        EXPECT_EQ(afterDry.at(i), 0)
            << "dry_run committed " << rowTables().at(i).toStdString() << " rows";
    }

    // A provisional rowid a later real run need not reuse is worse than no id,
    // because it looks durable.
    EXPECT_EQ(dry.value(QStringLiteral("project_id")).toInt(), 0);

    req.dryRun = false;
    const QJsonObject real = RoadmapMigrateVerb::run(storePath, req);
    ASSERT_TRUE(real.value(QStringLiteral("ok")).toBool())
        << real.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_FALSE(real.value(QStringLiteral("dry_run")).toBool());
    EXPECT_GT(real.value(QStringLiteral("project_id")).toInt(), 0);

    for (const QString &field : countFields()) {
        EXPECT_EQ(dry.value(field).toInt(), real.value(field).toInt())
            << "the preview and the real run disagree on " << field.toStdString();
    }

    // sources[].path is relative to the root the caller supplied, on both.
    for (const QJsonObject &env : {dry, real}) {
        const QJsonArray sources = env.value(QStringLiteral("sources")).toArray();
        ASSERT_EQ(sources.size(), 1);
        EXPECT_EQ(sources.at(0).toObject().value(QStringLiteral("path")).toString(),
                  QStringLiteral("ROADMAP.md"));
        EXPECT_EQ(sources.at(0).toObject().value(QStringLiteral("format")).toString(),
                  QStringLiteral("ants-v1"));
    }
}

// ---------------------------------------------------------------- INV-4 -----
//
// Stated as "unchanged", not "zero", because two of § 2.5's refusals REQUIRE
// rows to already exist: a re-run that changes this root's slug or name, and a
// slug owned by a different root, all presuppose a `project` row. Against those,
// a zero-count assertion is false for a correct implementation.

TEST(RoadmapMigrateVerb, Inv4NoRefusalTouchesARow) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = makeProjectRoot(dir, QStringLiteral("proj"), demoRoadmap());
    ASSERT_FALSE(root.isEmpty());
    const QString other = makeProjectRoot(dir, QStringLiteral("other"), demoRoadmap());
    ASSERT_FALSE(other.isEmpty());
    const QString storePath = dir.filePath(QStringLiteral("store.sqlite"));

    ASSERT_TRUE(RoadmapMigrateVerb::run(storePath, request(root))
                    .value(QStringLiteral("ok")).toBool());
    const QList<int> before = rowCounts(storePath);
    ASSERT_EQ(before.size(), rowTables().size());

    // An empty directory: findRoadmaps() finds nothing under it.
    const QString bare = dir.filePath(QStringLiteral("bare"));
    ASSERT_TRUE(QDir().mkpath(bare));

    struct Case {
        const char *what;
        const char *code;
        RoadmapMigrateVerb::Request req;
    };

    auto named = [&](const QString &slug, const QString &name) {
        return request(root, slug, name);
    };

    QList<Case> cases;
    cases.append({"empty project_name", "bad_args", named(QStringLiteral("demo"),
                                                          QStringLiteral("   "))});
    cases.append({"invalid export_slug", "bad_args",
                  named(QStringLiteral("Ants_Terminal"), QStringLiteral("Demo"))});
    cases.append({"no roadmap under the root", "no_roadmap",
                  request(QFileInfo(bare).canonicalFilePath())});
    cases.append({"the slug belongs to a different root", "slug_collision",
                  request(other, QStringLiteral("demo"))});
    cases.append({"a re-run changing this root's slug", "slug_collision",
                  named(QStringLiteral("renamed"), QStringLiteral("Demo"))});
    cases.append({"a re-run changing this root's project_name", "bad_args",
                  named(QStringLiteral("demo"), QStringLiteral("Renamed"))});

    // Step 8: load() validates `changedAt` BEFORE opening its transaction —
    // history.changed_at CHECKs the exact ISO-Z shape — so a malformed stamp
    // refuses and writes nothing. This is the one refusal that reaches load().
    {
        auto malformed = request(root);
        malformed.changedAt = QStringLiteral("not-a-timestamp");
        cases.append({"a malformed changed_at", "migrate_failed", malformed});
    }

    for (const Case &c : cases) {
        const QJsonObject env = RoadmapMigrateVerb::run(storePath, c.req);
        EXPECT_FALSE(env.value(QStringLiteral("ok")).toBool()) << c.what;
        EXPECT_EQ(env.value(QStringLiteral("code")).toString(),
                  QString::fromLatin1(c.code)) << c.what;
        EXPECT_FALSE(env.value(QStringLiteral("error")).toString().isEmpty())
            << c.what << ": a refusal must carry its reason";

        const QList<int> after = rowCounts(storePath);
        EXPECT_EQ(after, before)
            << c.what << " changed the store: before [" << describeCounts(before).toStdString()
            << "] after [" << describeCounts(after).toStdString() << "]";
    }

    // Step 5, against a path that is a DIRECTORY, so open() cannot succeed. It
    // is a different store by construction, so it cannot touch the one above —
    // the assertion here is the refusal code.
    {
        const QString asDir = dir.filePath(QStringLiteral("store-dir"));
        ASSERT_TRUE(QDir().mkpath(asDir));
        const QJsonObject env = RoadmapMigrateVerb::run(asDir, request(root));
        EXPECT_FALSE(env.value(QStringLiteral("ok")).toBool());
        EXPECT_EQ(env.value(QStringLiteral("code")).toString(),
                  QStringLiteral("store_failed"));
    }
}

// ---------------------------------------------------------------- INV-5 -----

TEST(RoadmapMigrateVerb, Inv5OneStampPerCall) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = makeProjectRoot(dir, QStringLiteral("proj"), demoRoadmap());
    ASSERT_FALSE(root.isEmpty());
    const QString storePath = dir.filePath(QStringLiteral("store.sqlite"));

    ASSERT_TRUE(RoadmapMigrateVerb::run(storePath, request(root))
                    .value(QStringLiteral("ok")).toBool());

    // The fixture must be a re-run over an EDITED source, not a first load:
    // Loader::recordHistory() is reached only from the field-update path, so a
    // first-ever migration into an empty store inserts items and appends no
    // `history` row at all — against which a COUNT(DISTINCT changed_at) guard
    // would pass or red for a reason unrelated to the one-stamp rule.
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"),
                          demoRoadmap(QStringLiteral("An edited open item."))));

    auto second = request(root);
    second.changedAt = QStringLiteral("2026-08-06T11:22:33Z");
    const QJsonObject env = RoadmapMigrateVerb::run(storePath, second);
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool())
        << env.value(QStringLiteral("error")).toString().toStdString();

    EXPECT_EQ(env.value(QStringLiteral("changed_at")).toString(), second.changedAt);
    EXPECT_TRUE(QRegularExpression(
                    QStringLiteral(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$)"))
                    .match(env.value(QStringLiteral("changed_at")).toString())
                    .hasMatch())
        << "changed_at must be the shape history.changed_at CHECKs";
    ASSERT_GT(env.value(QStringLiteral("history_rows")).toInt(), 0)
        << "the edited re-run wrote no history, so the guard below is vacuous";

    auto store = openStore(storePath, RoadmapStore::Access::Interactive);
    ASSERT_NE(store, nullptr);
    QSqlQuery q(store->db());
    ASSERT_TRUE(q.exec(QStringLiteral(
        "SELECT COUNT(DISTINCT changed_at) FROM history WHERE changed_at = '%1'")
                           .arg(second.changedAt)));
    ASSERT_TRUE(q.next());
    EXPECT_EQ(q.value(0).toInt(), 1);

    QSqlQuery all(store->db());
    ASSERT_TRUE(all.exec(QStringLiteral("SELECT COUNT(DISTINCT changed_at) FROM history")));
    ASSERT_TRUE(all.next());
    EXPECT_EQ(all.value(0).toInt(), 1)
        << "one call stamps once; a handler stamping per row would show more";
}

// ---------------------------------------------------------------- INV-6 -----
//
// File existence IS the right observable here, and only here: these two refuse
// at steps 1-2, before anything opens a store, so asserting no file appeared is
// what proves nothing was opened. Every other refusal follows store.open(),
// which legitimately creates the file (§ 2.3.1) — which is why INV-4 is stated
// at row level instead.

TEST(RoadmapMigrateVerb, Inv6ArgumentRefusalsOpenNoStore) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = makeProjectRoot(dir, QStringLiteral("proj"), demoRoadmap());
    ASSERT_FALSE(root.isEmpty());

    struct Leg { const char *what; QString slug, name, storeLeaf; };
    const QList<Leg> legs = {
        {"a caller-supplied export_slug is validated verbatim, never slugified",
         QStringLiteral("Ants_Terminal"), QStringLiteral("Demo"),
         QStringLiteral("slug-store.sqlite")},
        {"an all-whitespace project_name identifies nothing",
         QStringLiteral("demo"), QStringLiteral("   "),
         QStringLiteral("name-store.sqlite")},
    };

    for (const Leg &leg : legs) {
        // A path the TEST controls, so the leg is satisfiable on a machine
        // whose real store already exists.
        const QString storePath = dir.filePath(leg.storeLeaf);
        ASSERT_FALSE(QFile::exists(storePath));

        const QJsonObject env =
            RoadmapMigrateVerb::run(storePath, request(root, leg.slug, leg.name));
        EXPECT_FALSE(env.value(QStringLiteral("ok")).toBool()) << leg.what;
        EXPECT_EQ(env.value(QStringLiteral("code")).toString(),
                  QStringLiteral("bad_args")) << leg.what;
        EXPECT_FALSE(QFile::exists(storePath))
            << leg.what << ": an argument refusal must not open a store";
    }
}

// ---------------------------------------------------------------- INV-7 -----

TEST(RoadmapMigrateVerb, Inv7ReRunIsIdempotent) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = makeProjectRoot(dir, QStringLiteral("proj"), demoRoadmap());
    ASSERT_FALSE(root.isEmpty());
    const QString storePath = dir.filePath(QStringLiteral("store.sqlite"));

    const QJsonObject first = RoadmapMigrateVerb::run(storePath, request(root));
    ASSERT_TRUE(first.value(QStringLiteral("ok")).toBool())
        << first.value(QStringLiteral("error")).toString().toStdString();

    const QJsonObject second = RoadmapMigrateVerb::run(storePath, request(root));
    ASSERT_TRUE(second.value(QStringLiteral("ok")).toBool())
        << second.value(QStringLiteral("error")).toString().toStdString();

    // `elements_written` is the ONE count deliberately excluded: § 2.6 rebuilds
    // element rows wholesale, so it is non-zero even on an unchanged re-run.
    for (const QString &field : {QStringLiteral("items_inserted"),
                                 QStringLiteral("items_updated"),
                                 QStringLiteral("items_orphaned"),
                                 QStringLiteral("ids_allocated"),
                                 QStringLiteral("sections_written"),
                                 QStringLiteral("history_rows")}) {
        EXPECT_EQ(second.value(field).toInt(), 0)
            << "an unchanged re-run reported " << field.toStdString();
    }
    EXPECT_EQ(second.value(QStringLiteral("items_unchanged")).toInt(), 1);
    EXPECT_EQ(second.value(QStringLiteral("project_id")).toInt(),
              first.value(QStringLiteral("project_id")).toInt());

    const QList<int> counts = rowCounts(storePath);
    ASSERT_EQ(counts.size(), rowTables().size());
    EXPECT_EQ(counts.at(0), 1) << "a re-run must not add a second project row";
}

// ---------------------------------------------------------------- INV-8 -----
//
// Scoped to what this fixture can falsify. The stronger property — that a
// RUNNING session's consumers pick the migration up with no restart — lives in
// RemoteControl::roadmapStoreOrNull()'s "absence is NOT remembered" caching,
// which migratedProject() never touches and which this test would pass without.
// That property is ANTS-3793's and is already shipped.

TEST(RoadmapMigrateVerb, Inv8ResolvesThroughTheConsumerDispatch) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = makeProjectRoot(dir, QStringLiteral("proj"), demoRoadmap());
    ASSERT_FALSE(root.isEmpty());
    const QString storePath = dir.filePath(QStringLiteral("store.sqlite"));
    const QString markdown = readAll(root + QStringLiteral("/ROADMAP.md"));
    ASSERT_FALSE(markdown.isEmpty());

    {
        auto store = openStore(storePath, RoadmapStore::Access::Interactive);
        ASSERT_NE(store, nullptr);
        QString err;
        EXPECT_FALSE(RoadmapSource::migratedProject(*store, root, markdown, &err)
                         .has_value())
            << "no project row yet";
        EXPECT_TRUE(err.isEmpty()) << err.toStdString();
    }

    const QJsonObject env = RoadmapMigrateVerb::run(storePath, request(root));
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool())
        << env.value(QStringLiteral("error")).toString().toStdString();

    {
        auto store = openStore(storePath, RoadmapStore::Access::Interactive);
        ASSERT_NE(store, nullptr);
        QString err;
        const auto resolved = RoadmapSource::migratedProject(*store, root, markdown, &err);
        ASSERT_TRUE(resolved.has_value()) << err.toStdString();
        EXPECT_EQ(*resolved, qint64(env.value(QStringLiteral("project_id")).toInt()));
    }
}

// ---------------------------------------------------------------- INV-9 -----

TEST(RoadmapMigrateVerb, Inv9CreatedStoreIsOwnerOnly) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = makeProjectRoot(dir, QStringLiteral("proj"), demoRoadmap());
    ASSERT_FALSE(root.isEmpty());
    const QString storePath = dir.filePath(QStringLiteral("store.sqlite"));
    ASSERT_FALSE(QFile::exists(storePath));

    {
        // Under 022 the file would be 0644 by default, so 0600 can only come
        // from setOwnerOnlyPerms().
        ScopedUmask relaxed(022);
        const QJsonObject env = RoadmapMigrateVerb::run(storePath, request(root));
        ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool())
            << env.value(QStringLiteral("error")).toString().toStdString();
    }

    ASSERT_TRUE(QFile::exists(storePath));
    const QFile::Permissions perms = QFile::permissions(storePath);
    EXPECT_TRUE(perms.testFlag(QFile::ReadOwner));
    EXPECT_TRUE(perms.testFlag(QFile::WriteOwner));
    const QFile::Permissions forbidden =
        QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup |
        QFile::ReadOther | QFile::WriteOther | QFile::ExeOther;
    EXPECT_EQ(perms & forbidden, QFile::Permissions())
        << "the store carries visibility:internal items; it is 0600 or it leaks";

    // The store FILE only. SQLite removes -wal and -shm when the last
    // connection closes cleanly and run() closes its connection before it
    // returns, so a post-call check on them would stat files that no longer
    // exist. They ARE chmodded, by RoadmapStore::open(), and ANTS-3756 INV-17
    // is where that is asserted.
    EXPECT_FALSE(QFile::exists(storePath + QStringLiteral("-wal")))
        << "run() must release its connection before returning";
}

// --------------------------------------------------------------- INV-10 -----
//
// Both legs are needed because the two bounds are independent: capping the
// entry count bounds no bytes, `Note::detail` being a QString with no length
// rule of its own.

namespace {

// `kind_unmapped` is raised once per item whose `Kind:` value is outside the
// store's taxonomy, and its detail is the raw value — so one fixture family
// exercises both the entry cap (many items) and the byte cap (one long value).
QByteArray unmappedKindRoadmap(int items, const QString &kind) {
    QByteArray md =
        "<!-- ants-roadmap-format: 1 -->\n"
        "\n"
        "# Demo — Roadmap\n"
        "\n"
        "## Work\n"
        "\n";
    for (int i = 1; i <= items; ++i) {
        md += "- \xE2\x9C\x85 [DEMO-";
        md += QStringLiteral("%1").arg(i, 4, 10, QLatin1Char('0')).toUtf8();
        md += "] **Item ";
        md += QByteArray::number(i);
        md += ".**\n  Kind: ";
        md += kind.toUtf8();
        md += ".\n  Source: test.\n\n";
    }
    return md;
}

}  // namespace

TEST(RoadmapMigrateVerb, Inv10NotesAreBoundedOnBothAxes) {
    // (a) The entry cap. One note per offending line, 250 of them.
    {
        QTemporaryDir dir;
        ASSERT_TRUE(dir.isValid());
        const QString root = makeProjectRoot(
            dir, QStringLiteral("many"),
            unmappedKindRoadmap(250, QStringLiteral("notakind")));
        ASSERT_FALSE(root.isEmpty());

        const QJsonObject env = RoadmapMigrateVerb::run(
            dir.filePath(QStringLiteral("store.sqlite")), request(root));
        ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool())
            << env.value(QStringLiteral("error")).toString().toStdString();

        EXPECT_EQ(env.value(QStringLiteral("notes")).toArray().size(), 200);
        EXPECT_TRUE(env.value(QStringLiteral("notes_truncated")).toBool());
        EXPECT_GT(env.value(QStringLiteral("notes_count")).toInt(), 200)
            << "notes_count must stay the TRUE total, not the clipped one";
    }

    // (b) The byte cap. A capped array of uncapped strings bounds nothing.
    {
        QTemporaryDir dir;
        ASSERT_TRUE(dir.isValid());
        const QString longKind = QString(3000, QLatin1Char('z'));
        const QString root = makeProjectRoot(dir, QStringLiteral("long"),
                                             unmappedKindRoadmap(1, longKind));
        ASSERT_FALSE(root.isEmpty());

        const QJsonObject env = RoadmapMigrateVerb::run(
            dir.filePath(QStringLiteral("store.sqlite")), request(root));
        ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool())
            << env.value(QStringLiteral("error")).toString().toStdString();

        const QJsonArray notes = env.value(QStringLiteral("notes")).toArray();
        ASSERT_FALSE(notes.isEmpty());
        bool sawClipped = false;
        for (const QJsonValue &v : notes) {
            const QString detail = v.toObject().value(QStringLiteral("detail")).toString();
            EXPECT_LE(detail.size(), 2048)
                << "note detail is unbounded: " << detail.size() << " characters";
            if (detail.size() == 2048) {
                sawClipped = true;
                EXPECT_TRUE(detail.endsWith(QStringLiteral("…")));
            }
        }
        EXPECT_TRUE(sawClipped)
            << "the fixture raised no over-long note, so the cap is untested";
    }
}
