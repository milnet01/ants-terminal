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

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <sys/stat.h>

#include <memory>
#include <utility>

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
        // ANTS-4065 § 2.6's governed-column counter. Listed here so INV-3's
        // dry-run/real-run parity covers it: a counter that agreed only on the
        // real run would make the dry run's report a different report.
        QStringLiteral("items_updated_governed"),
        QStringLiteral("items_unchanged"), QStringLiteral("items_orphaned"),
        QStringLiteral("ids_allocated"),   QStringLiteral("sections_written"),
        // ANTS-4490 — sections_written's partner. `0 written` alone is
        // illegible: it is INV-7's proof of idempotence and reads as a counter
        // that never moved.
        QStringLiteral("sections_unchanged"),
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

    // 0 means ONE thing: this root has no project row yet. That is the truthful
    // answer here, and a provisional rowid a later real run need not reuse is
    // worse than no id, because it looks durable.
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

    // ANTS-4478, the third run: a dry run over an ALREADY-MIGRATED root reports
    // the real id. Three projects reported `project_id: 0` here, beside counts
    // that could only have been diffed against that project's real rows — so
    // the envelope proved the lookup had succeeded while the id said it had
    // not. The pre-amendment code fails this leg and passes the other two.
    auto again = request(root);
    again.dryRun = true;
    const QJsonObject dryAgain = RoadmapMigrateVerb::run(storePath, again);
    ASSERT_TRUE(dryAgain.value(QStringLiteral("ok")).toBool())
        << dryAgain.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_EQ(dryAgain.value(QStringLiteral("project_id")).toInt(),
              real.value(QStringLiteral("project_id")).toInt())
        << "a preview of an already-migrated project must name its real id: a "
           "caller scripting \"migrate only if not already present\" reads "
           "project_id > 0 as \"already migrated\"";
}

// ANTS-4479 / ANTS-4490 — INV-3's fourth run. The three runs above are all over
// a store with no prior rows or an unchanged root, so `items_updated` is 0 and
// `updated_items` is [] on both sides of every comparison there — and two empty
// arrays agreeing is not evidence. This pair is the only one in which either
// value is non-empty, and an implementation collecting them on the committed
// path alone passes every other leg. INV-5's pattern, for INV-5's reason.

TEST(RoadmapMigrateVerb, Inv3DryRunPreviewsAnEditedReRunTruly) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = makeProjectRoot(dir, QStringLiteral("proj"), demoRoadmap());
    ASSERT_FALSE(root.isEmpty());
    const QString storePath = dir.filePath(QStringLiteral("store.sqlite"));

    ASSERT_TRUE(RoadmapMigrateVerb::run(storePath, request(root))
                    .value(QStringLiteral("ok")).toBool());
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"),
                          demoRoadmap(QStringLiteral("An edited item."))));

    auto req = request(root);
    req.changedAt = QStringLiteral("2026-08-06T11:00:00Z");
    req.dryRun    = true;
    const QJsonObject dry = RoadmapMigrateVerb::run(storePath, req);
    ASSERT_TRUE(dry.value(QStringLiteral("ok")).toBool())
        << dry.value(QStringLiteral("error")).toString().toStdString();
    ASSERT_EQ(dry.value(QStringLiteral("items_updated")).toInt(), 1)
        << "the fixture edited nothing, so this case measures nothing";

    req.dryRun = false;
    const QJsonObject real = RoadmapMigrateVerb::run(storePath, req);
    ASSERT_TRUE(real.value(QStringLiteral("ok")).toBool())
        << real.value(QStringLiteral("error")).toString().toStdString();

    for (const QString &field : countFields()) {
        EXPECT_EQ(dry.value(field).toInt(), real.value(field).toInt())
            << "the preview and the real run disagree on " << field.toStdString();
    }
    EXPECT_EQ(dry.value(QStringLiteral("updated_items")).toArray(),
              real.value(QStringLiteral("updated_items")).toArray())
        << "the preview named different items from the run it previews";
    EXPECT_FALSE(dry.value(QStringLiteral("updated_items")).toArray().isEmpty())
        << "both are empty, so their equality is not evidence";
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

    // ANTS-4490 — what makes `sections_written: 0` readable as idempotence
    // rather than as a counter that never moved. Vestige reported the 0 as a
    // bug in its own right; it was not one, it was unreadable.
    // Against the FIRST run's sections_written rather than a literal: the plan
    // carries a synthetic section for the content above the first heading as
    // well as `## Work`, so a hard-coded 1 asserts the fixture's shape instead
    // of the invariant. What is being pinned is that the two counters partition
    // the plan's sections — all written on the first run, all unchanged on the
    // second.
    EXPECT_EQ(second.value(QStringLiteral("sections_unchanged")).toInt(),
              first.value(QStringLiteral("sections_written")).toInt())
        << "an unchanged re-run must match every section the first run wrote";
    EXPECT_GT(first.value(QStringLiteral("sections_written")).toInt(), 0)
        << "the fixture planned no sections, so this leg measures nothing";

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
        // ANTS-3863 — the seam takes a provider; fromMemory reads no disk, so
        // this case exercises exactly what it did before.
        auto text = RoadmapSource::RoadmapText::fromMemory(markdown);
        EXPECT_FALSE(RoadmapSource::migratedProject(*store, root, text, &err)
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
        auto text = RoadmapSource::RoadmapText::fromMemory(markdown);
        const auto resolved = RoadmapSource::migratedProject(*store, root, text, &err);
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

// --------------------------------------------------------------- INV-11 -----
//
// The verb writes no SOURCE file under req.projectRoot, and markdown_rewritten
// says so. Asserted as "no pre-existing file changed and the only new path is
// the store", never as "every hash unchanged": § 6's fixture puts the store
// under that same root and has the verb create it, so the flat form would red
// against a correct implementation. Excluding *.sqlite* from the hash set
// instead would hide a regression in the store's own location, which is what
// INV-6 and INV-9 rest on.

namespace {

// path -> sha256 of its contents, for every file under `dir`.
QHash<QString, QByteArray> hashTree(const QString &dir) {
    QHash<QString, QByteArray> out;
    QDirIterator it(dir, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        QCryptographicHash h(QCryptographicHash::Sha256);
        h.addData(&f);
        out.insert(QDir(dir).relativeFilePath(path), h.result());
    }
    return out;
}

}  // namespace

TEST(RoadmapMigrateVerb, Inv11WritesNoSourceFileUnderTheProjectRoot) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = makeProjectRoot(dir, QStringLiteral("proj"), demoRoadmap());
    ASSERT_FALSE(root.isEmpty());
    const QString storePath = root + QStringLiteral("/store.sqlite");

    const QHash<QString, QByteArray> before = hashTree(root);
    ASSERT_FALSE(before.isEmpty()) << "the fixture wrote nothing to hash";

    auto req = request(root);
    req.dryRun = true;
    const QJsonObject dry = RoadmapMigrateVerb::run(storePath, req);
    ASSERT_TRUE(dry.value(QStringLiteral("ok")).toBool())
        << dry.value(QStringLiteral("error")).toString().toStdString();

    req.dryRun = false;
    const QJsonObject real = RoadmapMigrateVerb::run(storePath, req);
    ASSERT_TRUE(real.value(QStringLiteral("ok")).toBool())
        << real.value(QStringLiteral("error")).toString().toStdString();

    // PRESENCE first, then the value. A missing key reads as false through
    // toBool(), so asserting the value alone passes against an envelope that
    // never carried the field — the whole point of it being a field rather
    // than a sentence in the description.
    for (const auto &leg : {std::make_pair("dry", dry), std::make_pair("real", real)}) {
        EXPECT_TRUE(leg.second.contains(QStringLiteral("markdown_rewritten")))
            << leg.first << " envelope carries no markdown_rewritten";
        EXPECT_FALSE(leg.second.value(QStringLiteral("markdown_rewritten")).toBool());
    }

    const QHash<QString, QByteArray> after = hashTree(root);
    for (auto i = before.constBegin(); i != before.constEnd(); ++i) {
        ASSERT_TRUE(after.contains(i.key()))
            << "the verb deleted " << i.key().toStdString();
        EXPECT_EQ(after.value(i.key()), i.value())
            << "the verb rewrote " << i.key().toStdString();
    }

    // The store is the ONE new path. Its -wal/-shm sidecars are gone by now:
    // SQLite removes both when run() closes its connection (INV-9).
    QStringList appeared;
    for (auto i = after.constBegin(); i != after.constEnd(); ++i)
        if (!before.contains(i.key()))
            appeared.append(i.key());
    appeared.sort();
    EXPECT_EQ(appeared, QStringList{QStringLiteral("store.sqlite")})
        << "new paths under the project root: "
        << appeared.join(QStringLiteral(", ")).toStdString();
}

// --------------------------------------------------------------- INV-12 -----
//
// store_backed agrees with the consumer dispatch on a COMMITTED run, and stays
// the format answer on a dry one. Leg (b) is the one that matters: a successful
// migration whose project is not store-backed says so in the envelope, which is
// the state Vestige could only detect by noticing which fields a later
// roadmap_query response did not carry.

namespace {

// A github-task-list roadmap, the shape
// tests/features/roadmap_migrate_read/fixtures/archives/declaredformat/ carries.
// The schema CHECK accepts the dialect, so this migrates ok:true — and is then
// served from markdown, by design (roadmapsource.cpp, "legitimately
// markdown-served").
QByteArray gfmRoadmap() {
    return "# Roadmap\n"
           "\n"
           "## 0.7.0 — in flight\n"
           "\n"
           "- [x] A github-task-list live bullet.\n"
           "- [ ] Another live one.\n";
}

}  // namespace

TEST(RoadmapMigrateVerb, Inv12StoreBackedAgreesWithTheConsumerDispatch) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString antsRoot = makeProjectRoot(dir, QStringLiteral("ants"), demoRoadmap());
    const QString gfmRoot  = makeProjectRoot(dir, QStringLiteral("gfm"), gfmRoadmap());
    ASSERT_FALSE(antsRoot.isEmpty());
    ASSERT_FALSE(gfmRoot.isEmpty());
    const QString storePath = dir.filePath(QStringLiteral("store.sqlite"));

    // (a) ants-v1 — store_backed, and the dispatch resolves it afterwards.
    const QJsonObject a =
        RoadmapMigrateVerb::run(storePath, request(antsRoot, QStringLiteral("ants"),
                                                   QStringLiteral("Ants")));
    ASSERT_TRUE(a.value(QStringLiteral("ok")).toBool())
        << a.value(QStringLiteral("error")).toString().toStdString();
    ASSERT_TRUE(a.contains(QStringLiteral("store_backed")))
        << "the envelope carries no store_backed, so every assertion on it "
           "below would pass by reading a missing key as false";
    EXPECT_TRUE(a.value(QStringLiteral("store_backed")).toBool());

    // (b) github-task-list — ok:true with real counts, and NOT store-backed.
    const QJsonObject b =
        RoadmapMigrateVerb::run(storePath, request(gfmRoot, QStringLiteral("gfm"),
                                                   QStringLiteral("Gfm")));
    ASSERT_TRUE(b.value(QStringLiteral("ok")).toBool())
        << b.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_GT(b.value(QStringLiteral("items_inserted")).toInt(), 0)
        << "the fixture produced no items, so this leg measures nothing";
    ASSERT_TRUE(b.contains(QStringLiteral("store_backed")));
    EXPECT_FALSE(b.value(QStringLiteral("store_backed")).toBool());

    {
        auto store = openStore(storePath, RoadmapStore::Access::Interactive);
        ASSERT_TRUE(store);
        auto antsText = RoadmapSource::RoadmapText::fromMemory(
            readAll(antsRoot + QStringLiteral("/ROADMAP.md")));
        auto gfmText = RoadmapSource::RoadmapText::fromMemory(
            readAll(gfmRoot + QStringLiteral("/ROADMAP.md")));
        EXPECT_TRUE(RoadmapSource::migratedProject(*store, antsRoot, antsText)
                        .has_value())
            << "an ants-v1 project did not resolve through the consumer dispatch";
        EXPECT_FALSE(RoadmapSource::migratedProject(*store, gfmRoot, gfmText)
                         .has_value())
            << "a github-task-list project resolved from the store";
    }

    // (c) Both roots again under dry_run, asserting store_backed ALONE.
    // migratedProject() is deliberately not asserted here: nothing commits, so
    // it is nullopt for both while store_backed stays true for the first — the
    // two disagree on purpose, and a leg written as "the same values as (a) and
    // (b)" would red against a correct implementation.
    for (const auto &leg : {std::make_pair(antsRoot, true), std::make_pair(gfmRoot, false)}) {
        auto req = request(leg.first, leg.first == antsRoot ? QStringLiteral("ants")
                                                            : QStringLiteral("gfm"),
                           leg.first == antsRoot ? QStringLiteral("Ants")
                                                 : QStringLiteral("Gfm"));
        req.dryRun = true;
        const QJsonObject env = RoadmapMigrateVerb::run(storePath, req);
        ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool())
            << env.value(QStringLiteral("error")).toString().toStdString();
        ASSERT_TRUE(env.contains(QStringLiteral("store_backed")));
        EXPECT_EQ(env.value(QStringLiteral("store_backed")).toBool(), leg.second)
            << "a dry run changed store_backed, which answers a question about "
               "the source DIALECT — a rollback cannot change that";
    }
}

// --------------------------------------------------------------- INV-13 -----
//
// updated_items names exactly the items items_updated counted, with the fields
// that changed. Breaks when the array is filled from the plan rather than from
// the write path: every matched item would appear, and the array would say
// nothing the count does not.

namespace {

// N items, each headline suffixed so a second render can change all of them.
QByteArray manyItemRoadmap(int n, const QString &headlineSuffix) {
    QByteArray md =
        "<!-- ants-roadmap-format: 1 -->\n"
        "\n"
        "# Demo — Roadmap\n"
        "\n"
        "## Work\n"
        "\n";
    for (int i = 1; i <= n; ++i) {
        md += "- \xF0\x9F\x93\x8B [DEMO-";
        md += QStringLiteral("%1").arg(i, 4, 10, QLatin1Char('0')).toUtf8();
        md += "] **Item ";
        md += QByteArray::number(i);
        md += " ";
        md += headlineSuffix.toUtf8();
        md += "**\n"
              "  Layman: A thing.\n"
              "  Kind: implement.\n"
              "  Source: test.\n";
    }
    return md;
}

}  // namespace

TEST(RoadmapMigrateVerb, Inv13UpdatedItemsNamesWhatChanged) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = makeProjectRoot(dir, QStringLiteral("proj"),
                                         manyItemRoadmap(3, QStringLiteral("first")));
    ASSERT_FALSE(root.isEmpty());
    const QString storePath = dir.filePath(QStringLiteral("store.sqlite"));

    const QJsonObject first = RoadmapMigrateVerb::run(storePath, request(root));
    ASSERT_TRUE(first.value(QStringLiteral("ok")).toBool())
        << first.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_TRUE(first.value(QStringLiteral("updated_items")).toArray().isEmpty())
        << "a first migration inserts; it updates nothing";

    // Change two of the three: one headline, one status.
    QByteArray edited = manyItemRoadmap(3, QStringLiteral("first"));
    edited.replace("**Item 2 first**", "**Item 2 second**");
    edited.replace("- \xF0\x9F\x93\x8B [DEMO-0003]", "- \xE2\x9C\x85 [DEMO-0003]");
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"), edited));

    auto req2 = request(root);
    req2.changedAt = QStringLiteral("2026-08-06T11:00:00Z");
    const QJsonObject second = RoadmapMigrateVerb::run(storePath, req2);
    ASSERT_TRUE(second.value(QStringLiteral("ok")).toBool())
        << second.value(QStringLiteral("error")).toString().toStdString();

    EXPECT_EQ(second.value(QStringLiteral("items_updated")).toInt(), 2);
    const QJsonArray updated = second.value(QStringLiteral("updated_items")).toArray();
    ASSERT_EQ(updated.size(), 2)
        << QString::fromUtf8(QJsonDocument(updated).toJson()).toStdString();

    QHash<QString, QStringList> byId;
    for (const QJsonValue &v : updated) {
        const QJsonObject o = v.toObject();
        QStringList fields;
        for (const QJsonValue &f : o.value(QStringLiteral("fields")).toArray())
            fields.append(f.toString());
        fields.sort();
        byId.insert(o.value(QStringLiteral("id")).toString(), fields);
    }
    ASSERT_TRUE(byId.contains(QStringLiteral("DEMO-0002"))) << "the edited headline is missing";
    ASSERT_TRUE(byId.contains(QStringLiteral("DEMO-0003"))) << "the edited status is missing";
    EXPECT_EQ(byId.value(QStringLiteral("DEMO-0002")),
              QStringList{QStringLiteral("headline")});
    EXPECT_EQ(byId.value(QStringLiteral("DEMO-0003")),
              QStringList{QStringLiteral("status")});
    EXPECT_TRUE(second.contains(QStringLiteral("updated_items_truncated")));
    EXPECT_FALSE(second.value(QStringLiteral("updated_items_truncated")).toBool());
}

TEST(RoadmapMigrateVerb, Inv13UpdatedItemsIsBoundedAt200) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const int kItems = 205;
    const QString root = makeProjectRoot(dir, QStringLiteral("proj"),
                                         manyItemRoadmap(kItems, QStringLiteral("first")));
    ASSERT_FALSE(root.isEmpty());
    const QString storePath = dir.filePath(QStringLiteral("store.sqlite"));

    ASSERT_TRUE(RoadmapMigrateVerb::run(storePath, request(root))
                    .value(QStringLiteral("ok")).toBool());
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"),
                          manyItemRoadmap(kItems, QStringLiteral("second"))));

    auto req2 = request(root);
    req2.changedAt = QStringLiteral("2026-08-06T11:00:00Z");
    const QJsonObject env = RoadmapMigrateVerb::run(storePath, req2);
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool())
        << env.value(QStringLiteral("error")).toString().toStdString();

    // One bound, not notes[]'s two: an entry here is an id and a short column
    // list, so capping the entries bounds the bytes.
    EXPECT_EQ(env.value(QStringLiteral("updated_items")).toArray().size(), 200);
    EXPECT_TRUE(env.value(QStringLiteral("updated_items_truncated")).toBool());
    EXPECT_EQ(env.value(QStringLiteral("items_updated")).toInt(), kItems)
        << "items_updated must stay the TRUE total, not the clipped one";
}

// --------------------------------------------------------------- INV-14 -----
//
// ANTS-4600 — § 2.5 step 0b. The machine-global store acquired a project whose
// root was a session scratchpad under /tmp: 33 items, every id a byte-identical
// copy of LottoTracker's, and a directory that no longer exists. It inflated
// every machine-wide surface (`roadmap_query mode:"report" scope:"all"`, the
// ANTS-4585 survey) by one project's worth of a project that is not there.
//
// registerProject()'s INV-8 does not catch it: that refuses a root which does
// not CANONICALISE, and the scratchpad existed at migration time. Registration
// is what made it permanent, so the guard has to sit at registration.
//
// Two legs, because the predicate and its wiring fail independently — a correct
// predicate nobody calls refuses nothing. The behavioural half is a source
// scrape rather than a call, for INV-4's stated reason: the handler is
// RemoteControl's and test_core cannot link it (roadmapmigrateverb.h).

TEST(RoadmapMigrateVerb, Inv14TransientRootIsRefused) {
    // The temp root itself and anything beneath it.
    const QString tmp = QFileInfo(QDir::tempPath()).canonicalFilePath();
    ASSERT_FALSE(tmp.isEmpty()) << "no canonical temp dir; the guard is inert here";
    EXPECT_TRUE(RoadmapMigrateVerb::isTransientRoot(tmp));

    QTemporaryDir scratch;
    ASSERT_TRUE(scratch.isValid());
    const QString scratchRoot = QFileInfo(scratch.path()).canonicalFilePath();
    EXPECT_TRUE(RoadmapMigrateVerb::isTransientRoot(scratchRoot))
        << scratchRoot.toStdString() << " is under " << tmp.toStdString();

    // A real project root is not transient. ANTS_SRC_DIR is this checkout's
    // src/, which no CI runner puts under the temp dir.
    const QString srcDir = QFileInfo(QStringLiteral(ANTS_SRC_DIR)).canonicalFilePath();
    ASSERT_FALSE(srcDir.isEmpty());
    EXPECT_FALSE(RoadmapMigrateVerb::isTransientRoot(srcDir));

    // A SIBLING whose name merely starts with the temp dir's is not under it.
    // A bare startsWith() would match it and refuse a legitimate root.
    EXPECT_FALSE(RoadmapMigrateVerb::isTransientRoot(tmp + QStringLiteral("foo")));

    // An empty root disables the guard rather than matching everything: the
    // caller-cwd refusal above it already owns that case.
    EXPECT_FALSE(RoadmapMigrateVerb::isTransientRoot(QString()));
}

TEST(RoadmapMigrateVerb, Inv14HandlerWiresTheGuard) {
    const QString handlerPath =
        QStringLiteral(ANTS_SRC_DIR) + QStringLiteral("/remotecontrol_roadmap_migrate.cpp");
    QFile f(handlerPath);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly)) << handlerPath.toStdString();

    QStringList code;
    const QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (line.trimmed().startsWith(QStringLiteral("//")))
            continue;
        code.append(line);
    }
    const QString text = code.join(QLatin1Char('\n'));

    EXPECT_TRUE(text.contains(QStringLiteral("isTransientRoot")))
        << "the handler must consult the guard; a predicate nobody calls "
           "registers the next scratchpad exactly as before";
    EXPECT_TRUE(text.contains(QStringLiteral("transient_root")))
        << "the refusal must carry the `transient_root` code "
           "(docs/standards/mcp-error-codes.md § 1)";
}

// ---------------------------------------------------------- ANTS-4617 -------
//
// op:"deregister" — the inverse the catalogue never had. Migrating a scratch
// copy to test something destructive in isolation is the careful instinct, and
// it left a permanent row that roadmap_query mode:"report" scope:"all" sums
// into machine-wide figures forever. The store is machine-global, so the
// incentive ran the wrong way.

// Rows go from every table, and the project is gone.
TEST(RoadmapMigrateVerb, Ants4617DeregisterRemovesEveryTablesRows) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = makeProjectRoot(dir, QStringLiteral("proj"), demoRoadmap());
    ASSERT_FALSE(root.isEmpty());
    const QString storePath = dir.filePath(QStringLiteral("store.sqlite"));
    ASSERT_TRUE(RoadmapMigrateVerb::run(storePath, request(root))
                    .value(QStringLiteral("ok")).toBool());

    RoadmapMigrateVerb::DeregisterRequest d;
    d.projectRoot = root;
    d.confirm     = true;   // the root still exists; see the guard case below
    const QJsonObject env = RoadmapMigrateVerb::deregister(storePath, d);

    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool())
        << env.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_TRUE(env.value(QStringLiteral("deregistered")).toBool());
    EXPECT_GT(env.value(QStringLiteral("items")).toInt(), 0)
        << "ANTS-4617: the counts are the caller's only account of what went";

    const QList<int> after = rowCounts(storePath);
    ASSERT_EQ(after.size(), rowTables().size());
    for (int i = 0; i < after.size(); ++i) {
        EXPECT_EQ(after.at(i), 0)
            << "ANTS-4617: " << rowTables().at(i).toStdString()
            << " still holds rows — " << describeCounts(after).toStdString();
    }
}

// THE ONE THAT MATTERS. The store is machine-global and held 17 projects when
// this was filed, so a delete that reached past its own project would be far
// worse than the clutter it was written to remove.
TEST(RoadmapMigrateVerb, Ants4617DeregisterLeavesSiblingProjectsIntact) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString storePath = dir.filePath(QStringLiteral("store.sqlite"));

    const QString doomed = makeProjectRoot(dir, QStringLiteral("doomed"), demoRoadmap());
    const QString keeper = makeProjectRoot(dir, QStringLiteral("keeper"), demoRoadmap());
    ASSERT_FALSE(doomed.isEmpty());
    ASSERT_FALSE(keeper.isEmpty());
    ASSERT_TRUE(RoadmapMigrateVerb::run(
        storePath, request(doomed, QStringLiteral("doomed"), QStringLiteral("Doomed")))
                    .value(QStringLiteral("ok")).toBool());
    ASSERT_TRUE(RoadmapMigrateVerb::run(
        storePath, request(keeper, QStringLiteral("keeper"), QStringLiteral("Keeper")))
                    .value(QStringLiteral("ok")).toBool());
    const QList<int> both = rowCounts(storePath);

    RoadmapMigrateVerb::DeregisterRequest d;
    d.projectRoot = doomed;
    d.confirm     = true;
    ASSERT_TRUE(RoadmapMigrateVerb::deregister(storePath, d)
                    .value(QStringLiteral("ok")).toBool());

    // The keeper's project row survives, and so does everything hanging off it:
    // exactly half the rows should have gone, since the two fixtures are equal.
    auto store = openStore(storePath, RoadmapStore::Access::Interactive);
    ASSERT_NE(store, nullptr);
    QString err;
    const auto kept = store->readProjectBySlug(QStringLiteral("keeper"), &err);
    ASSERT_TRUE(kept.has_value()) << err.toStdString();
    const auto gone = store->readProjectBySlug(QStringLiteral("doomed"), &err);
    EXPECT_FALSE(gone.has_value()) << "ANTS-4617: the doomed row must be gone";

    const auto items = store->readItems(kept->projectId, &err);
    ASSERT_TRUE(items.has_value()) << err.toStdString();
    EXPECT_GT(items->size(), 0)
        << "ANTS-4617: the sibling's items were taken with the doomed project";

    const QList<int> left = rowCounts(storePath);
    ASSERT_EQ(left.size(), both.size());
    for (int i = 0; i < left.size(); ++i) {
        EXPECT_EQ(left.at(i), both.at(i) / 2)
            << "ANTS-4617: " << rowTables().at(i).toStdString()
            << " — the delete reached past its own project. before="
            << describeCounts(both).toStdString()
            << " after=" << describeCounts(left).toStdString();
    }
}

// The guard. Deregistering a LIVE project is data loss with no undo: the store
// is primary and ROADMAP.md is its render, so the rows are the only copy of the
// history, relationships and citations the file does not carry.
TEST(RoadmapMigrateVerb, Ants4617RefusesWhileTheRootStillExists) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = makeProjectRoot(dir, QStringLiteral("proj"), demoRoadmap());
    ASSERT_FALSE(root.isEmpty());
    const QString storePath = dir.filePath(QStringLiteral("store.sqlite"));
    ASSERT_TRUE(RoadmapMigrateVerb::run(storePath, request(root))
                    .value(QStringLiteral("ok")).toBool());
    const QList<int> before = rowCounts(storePath);

    RoadmapMigrateVerb::DeregisterRequest d;
    d.projectRoot = root;          // confirm deliberately absent
    const QJsonObject env = RoadmapMigrateVerb::deregister(storePath, d);

    EXPECT_FALSE(env.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(env.value(QStringLiteral("code")).toString(),
              QStringLiteral("confirm_required"));
    EXPECT_EQ(rowCounts(storePath), before)
        << "ANTS-4617: a refused deregister must delete nothing";
}

// An ABSENT root is the case the item was filed for — a scratch project whose
// files are long gone — and it needs no ceremony. Keyed by slug, because that
// is all a caller pruning such a row still has.
TEST(RoadmapMigrateVerb, Ants4617AbsentRootNeedsNoConfirmAndSlugIsAKey) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = makeProjectRoot(dir, QStringLiteral("scratch"), demoRoadmap());
    ASSERT_FALSE(root.isEmpty());
    const QString storePath = dir.filePath(QStringLiteral("store.sqlite"));
    ASSERT_TRUE(RoadmapMigrateVerb::run(
        storePath, request(root, QStringLiteral("scratch"), QStringLiteral("Scratch")))
                    .value(QStringLiteral("ok")).toBool());

    ASSERT_TRUE(QDir(root).removeRecursively())
        << "the scratchpad this models is deleted minutes later";

    RoadmapMigrateVerb::DeregisterRequest d;
    d.exportSlug = QStringLiteral("scratch");   // no root, no confirm
    const QJsonObject env = RoadmapMigrateVerb::deregister(storePath, d);

    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool())
        << env.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_FALSE(env.value(QStringLiteral("root_exists")).toBool());
    EXPECT_TRUE(env.value(QStringLiteral("deregistered")).toBool());
}

// dry_run reports what would go and deletes nothing. This is the one verb whose
// preview a caller runs precisely because they are afraid of the real call, so
// a preview that performed the delete to measure it would be the opposite of
// reassuring — the count comes from a read, not from a rolled-back delete.
TEST(RoadmapMigrateVerb, Ants4617DryRunDeletesNothing) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = makeProjectRoot(dir, QStringLiteral("proj"), demoRoadmap());
    ASSERT_FALSE(root.isEmpty());
    const QString storePath = dir.filePath(QStringLiteral("store.sqlite"));
    ASSERT_TRUE(RoadmapMigrateVerb::run(storePath, request(root))
                    .value(QStringLiteral("ok")).toBool());
    const QList<int> before = rowCounts(storePath);

    RoadmapMigrateVerb::DeregisterRequest d;
    d.projectRoot = root;
    d.confirm     = true;
    d.dryRun      = true;
    const QJsonObject env = RoadmapMigrateVerb::deregister(storePath, d);

    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool())
        << env.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_TRUE(env.value(QStringLiteral("dry_run")).toBool());
    EXPECT_GT(env.value(QStringLiteral("items")).toInt(), 0)
        << "ANTS-4617: a preview that reports nothing is not a preview";
    EXPECT_FALSE(env.contains(QStringLiteral("deregistered")))
        << "ANTS-4463: no past-tense field on a preview";
    EXPECT_EQ(rowCounts(storePath), before)
        << "ANTS-4617: a preview must delete nothing";
}

// An unknown key is `not_found`, not a silent success. A prune loop that read
// ok:true for a project it never removed would report a clean store it had not
// cleaned.
TEST(RoadmapMigrateVerb, Ants4617UnknownProjectIsNotFound) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString storePath = dir.filePath(QStringLiteral("store.sqlite"));
    const QString root = makeProjectRoot(dir, QStringLiteral("proj"), demoRoadmap());
    ASSERT_TRUE(RoadmapMigrateVerb::run(storePath, request(root))
                    .value(QStringLiteral("ok")).toBool());

    RoadmapMigrateVerb::DeregisterRequest d;
    d.exportSlug = QStringLiteral("never-registered");
    const QJsonObject env = RoadmapMigrateVerb::deregister(storePath, d);
    EXPECT_FALSE(env.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(env.value(QStringLiteral("code")).toString(),
              QStringLiteral("not_found"));
}

// ---------------------------------------------------- ANTS-4621 schema decl --

// The `roadmap_migrate` entry of the tools/list array, from `t["name"] =
// "roadmap_migrate"` to its `tools.append(t)`. Scraped rather than built: the
// assembled array lives inside ClaudeIntegration, which drags MainWindow behind
// it, and this bundle links neither — the same reason INV-2(b) is a grep.
static QString migrateToolBlock() {
    QFile f(QStringLiteral(ANTS_SRC_DIR) + QStringLiteral("/claudeintegration.cpp"));
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    const QString all = QString::fromUtf8(f.readAll());
    const int start = all.indexOf(QStringLiteral("t[\"name\"] = \"roadmap_migrate\";"));
    if (start < 0)
        return QString();
    const int end = all.indexOf(QStringLiteral("tools.append(t);"), start);
    if (end < 0)
        return QString();
    return all.mid(start, end - start);
}

// ANTS-4621 — every argument the HANDLER reads must be declared in the verb's
// schema. `op` and `confirm` were not: the schema sets
// additionalProperties:false, so a validating client rejects the call outright,
// and the permissive path is worse than the strict one — ANTS-2175's
// `ignored_args` advisory names `op` as ignored while that very argument is
// what selected the deregister branch. A caller reading the envelope is told
// its argument did nothing, by the call the argument just steered.
//
// Falsifiable against the pre-fix tree: both assertions fail there.
TEST(RoadmapMigrateVerb, Ants4621SchemaDeclaresDeregisterArgs) {
    const QString block = migrateToolBlock();
    ASSERT_FALSE(block.isEmpty())
        << "ANTS-4621: roadmap_migrate tool block not found in claudeintegration.cpp";

    EXPECT_TRUE(block.contains(QStringLiteral("props[\"op\"]")))
        << "ANTS-4621: cmdRoadmapMigrate reads req[\"op\"] to select deregister, "
           "so the schema must declare it";
    EXPECT_TRUE(block.contains(QStringLiteral("props[\"confirm\"]")))
        << "ANTS-4621: deregister's confirm_required refusal is only clearable "
           "by confirm:true, so the schema must declare it";

    // The declaration has to carry the value that reaches the branch, else a
    // client offering completions never surfaces it.
    EXPECT_TRUE(block.contains(QStringLiteral("deregister")))
        << "ANTS-4621: op's enum must contain \"deregister\"";
}

// The two names the schema declares are exactly the two the handler reads.
// Guards the reverse drift: a later op added to the handler and not to the
// schema lands back in `ignored_args` with no test to catch it.
TEST(RoadmapMigrateVerb, Ants4621HandlerReadsOnlyDeclaredArgs) {
    QFile f(QStringLiteral(ANTS_SRC_DIR)
            + QStringLiteral("/remotecontrol_roadmap_migrate.cpp"));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QStringList lines =
        QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));

    static const QRegularExpression reValue(
        QStringLiteral("req\\.value\\(QStringLiteral\\(\"([a-z_]+)\"\\)\\)"));
    QStringList read;
    for (const QString &line : lines) {
        if (line.trimmed().startsWith(QStringLiteral("//")))
            continue;
        auto it = reValue.globalMatch(line);
        while (it.hasNext()) {
            const QString key = it.next().captured(1);
            if (!read.contains(key))
                read.append(key);
        }
    }
    read.sort();
    ASSERT_FALSE(read.isEmpty()) << "ANTS-4621: scrape found no req.value() calls";

    const QString block = migrateToolBlock();
    ASSERT_FALSE(block.isEmpty());
    for (const QString &key : read) {
        if (key == QStringLiteral("caller_cwd"))
            continue;   // universal dispatch-layer arg (ANTS-2175 INV-2)
        EXPECT_TRUE(block.contains(QStringLiteral("props[\"%1\"]").arg(key)))
            << "ANTS-4621: the handler reads \"" << key.toStdString()
            << "\" but the schema does not declare it — additionalProperties is "
               "false, and ignored_args will call it ignored";
    }
}
