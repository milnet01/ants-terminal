// Feature-conformance test for ANTS-4491 — roadmap_log op:"convert": re-import
// a github-task-list roadmap and republish it as canonical ants-v1, atomically.
// See tests/features/roadmap_convert/spec.md.
//
// The atomicity is the subject. migratedProject() dispatches on the live file's
// detected format and refuses a disagreement with the stored source_format, so
// writing either half alone leaves every read refusing.

#include "../../_support/xdg_guard.h"

#include "remotecontrol.h"
#include "roadmapmigrate.h"
#include "roadmapmigrateload.h"
#include "roadmapparse.h"
#include "roadmapstore.h"
#include "roadmapwrite.h"

#include <gtest/gtest.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

#include <memory>

namespace {

// ------------------------------------------------------------- fixtures ----

// A github-task-list roadmap. Every open bullet carries a Layman line: the
// render's INV-5 gate judges every item a write touches, and a convert touches
// all of them, so one bullet without it refuses the whole run — which is what
// failedConvertIsInert uses deliberately.
QByteArray gfmRoadmap() {
    return QByteArray(
        "# Demo Roadmap\n"
        "\n"
        "## To Do\n"
        "\n"
        "- [ ] The first checklist bullet.\n"
        "  Layman: A plain-language line.\n"
        "  Kind: chore.\n"
        "  Source: ants-4491-test.\n"
        "- [ ] The second checklist bullet.\n"
        "  Layman: A plain-language line.\n"
        "  Kind: chore.\n"
        "  Source: ants-4491-test.\n"
        "- [x] The third checklist bullet, already done.\n"
        "  Layman: A plain-language line.\n"
        "  Kind: chore.\n"
        "  Source: ants-4491-test.\n");
}

// The good fixture plus one NEW open bullet carrying no Layman line.
//
// The gate judges only what the write TOUCHED (`itemsWrittenSinceBegin()`), so
// removing a Layman line from an already-migrated bullet is not enough: the
// convert re-matches it, writes nothing, and the item never enters the gate's
// scope. A bullet the store has not seen is INSERTED, which puts it in scope
// and allocates it an id — which is also what makes INV-4's "no id burnt"
// assertion mean anything.
QByteArray gfmRoadmapPlusUnlaymanned() {
    return gfmRoadmap()
           + QByteArray("- [ ] A newly added bullet with no Layman line.\n"
                        "  Kind: chore.\n"
                        "  Source: ants-4491-test.\n");
}

// Two Pass headings and two Status markers and no emoji bullets. With fewer,
// detectRoadmapFormat() falls to its ants-v1 default and the case would
// exercise the accepted path instead of the refusal.
QByteArray passHeadingsRoadmap() {
    return QByteArray(
        "# Demo Roadmap\n"
        "\n"
        "#### Pass 1.1 The first pass.\n"
        "- **Status**: done\n"
        "\n"
        "#### Pass 1.2 The second pass.\n"
        "- **Status**: todo\n");
}

bool writeFile(const QString &path, const QByteArray &body) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

QByteArray readFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    const QByteArray b = f.readAll();
    f.close();
    return b;
}

QByteArray hashOf(const QString &path) {
    return QCryptographicHash::hash(readFile(path), QCryptographicHash::Sha256);
}

// NEVER default-construct RoadmapStore: defaultPath() resolves the developer's
// REAL store under XDG_DATA_HOME. Every case here redirects that first.
std::unique_ptr<RoadmapStore> openStore() {
    auto store = std::make_unique<RoadmapStore>(RoadmapStore::defaultPath(),
                                                RoadmapStore::kDefaultHistoryCapBytes,
                                                RoadmapStore::Access::Bulk);
    QString err;
    if (!store->open(&err)) {
        ADD_FAILURE() << "store open: " << err.toStdString();
        return nullptr;
    }
    return store;
}

QString seed(ants_test::XdgGuard &guard, const QTemporaryDir &tmp,
             const QByteArray &body, const char *leaf = "proj") {
    guard.setEnv("XDG_DATA_HOME",
                 QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
    const QString raw = QDir(tmp.path()).filePath(QString::fromLatin1(leaf));
    if (!writeFile(raw + QStringLiteral("/ROADMAP.md"), body))
        return QString();
    return QFileInfo(raw).canonicalFilePath();
}

// Registers the project the way roadmap_migrate does. The convert refuses a
// project the store does not hold, deliberately: migrating implicitly would
// register a project nobody asked to register.
bool migrate(const QString &root, const char *stamp = "2026-09-21T10:00:00Z") {
    auto store = openStore();
    if (!store)
        return false;
    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    if (!disc) {
        ADD_FAILURE() << "findRoadmaps: " << err.toStdString();
        return false;
    }
    const QString leaf = QFileInfo(root).fileName();
    const auto plan = RoadmapMigrate::planFrom(*disc, leaf, leaf);
    RoadmapMigrateLoad::Options opts;
    opts.changedAt   = QString::fromLatin1(stamp);
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(*store, plan, opts);
    if (!out.ok)
        ADD_FAILURE() << "migration load: " << out.error.toStdString();
    return out.ok;
}

QJsonObject convert(const QString &root, bool dryRun = false) {
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("op")]         = QStringLiteral("convert");
    if (dryRun)
        req[QStringLiteral("dry_run")] = true;
    RemoteControl rc(nullptr);
    return rc.cmdRoadmapLogConvertForTest(req).object();
}

QString detectedFormatOf(const QString &roadmapPath) {
    const QString text = QString::fromUtf8(readFile(roadmapPath));
    return RoadmapParse::detectRoadmapFormat(text.split(QLatin1Char('\n')));
}

QString storedFormatOf(const QString &root) {
    auto store = openStore();
    if (!store)
        return QString();
    QString err;
    const auto row = store->readProjectByRoot(root, &err);
    if (!row) {
        ADD_FAILURE() << "readProjectByRoot: " << err.toStdString();
        return QString();
    }
    return row->sourceFormat;
}

int itemCountOf(const QString &root) {
    auto store = openStore();
    if (!store)
        return -1;
    QString err;
    const auto row = store->readProjectByRoot(root, &err);
    if (!row)
        return -1;
    const auto items = store->listItems(row->projectId, &err);
    return items ? int(items->size()) : -1;
}

// ANTS-5256 — forces commitAndRender() to abort in the window AFTER mutate()
// has run and BEFORE the store commits. RAII so an assertion failure mid-case
// cannot leak a `true` into every later case in this binary.
//
// INV-2 and INV-4 used to force that abort with the render's Layman gate. This
// item EXEMPTS op:"convert" from that gate, so both cases would have gone green
// by losing their trigger rather than by holding — a silent pass on the two
// invariants guarding the irreversible half of the op. The seam is named for
// the window instead of borrowed from a business rule, so the next rule change
// cannot repeat it.
class ForcePostMutateFail {
public:
    ForcePostMutateFail()  { RoadmapWrite::setForcePostMutateFailForTest(true); }
    ~ForcePostMutateFail() { RoadmapWrite::setForcePostMutateFailForTest(false); }
    ForcePostMutateFail(const ForcePostMutateFail &)            = delete;
    ForcePostMutateFail &operator=(const ForcePostMutateFail &) = delete;
};

}  // namespace

// ----------------------------------------------------------------- INV-1 ----

TEST(RoadmapConvert, formatsAgree) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seed(guard, tmp, gfmRoadmap());
    ASSERT_FALSE(root.isEmpty());
    const QString roadmap = root + QStringLiteral("/ROADMAP.md");
    ASSERT_EQ(detectedFormatOf(roadmap), QStringLiteral("github-task-list"))
        << "setup: the fixture must start as github-task-list";
    ASSERT_TRUE(migrate(root));

    const QJsonObject resp = convert(root);
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();

    EXPECT_EQ(detectedFormatOf(roadmap), QStringLiteral("ants-v1"))
        << "the file was not republished in the target dialect";
    EXPECT_EQ(storedFormatOf(root), QStringLiteral("ants-v1"))
        << "the store still records the old dialect, so every read now refuses";
}

// ----------------------------------------------------------------- INV-2 ----

TEST(RoadmapConvert, failedConvertIsInert) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seed(guard, tmp, gfmRoadmap());
    ASSERT_FALSE(root.isEmpty());
    const QString roadmap = root + QStringLiteral("/ROADMAP.md");
    ASSERT_TRUE(migrate(root));
    // Added AFTER the migration, so the convert has to insert it.
    ASSERT_TRUE(writeFile(roadmap, gfmRoadmapPlusUnlaymanned()));

    const QByteArray before       = readFile(roadmap);
    const QString    formatBefore = storedFormatOf(root);
    ASSERT_FALSE(before.isEmpty());

    QJsonObject resp;
    {
        // ANTS-5256 — the forced abort fires AFTER mutate() and BEFORE the
        // commit, which is the window this invariant is about. Scoped so the
        // flag is cleared before the assertions below read the store back.
        const ForcePostMutateFail forceFail;
        resp = convert(root);
    }
    ASSERT_FALSE(resp.value(QStringLiteral("ok")).toBool())
        << "the forced post-mutate failure must refuse: "
        << QJsonDocument(resp).toJson().toStdString();
    // Named, not merely "a refusal". Any EARLIER refusal (a store that would
    // not open, a project not registered) would satisfy a bare ASSERT_FALSE
    // while testing nothing — the mutation has to have happened for its
    // rollback to be worth asserting.
    ASSERT_EQ(resp.value(QStringLiteral("code")).toString().toStdString(),
              std::string("store_failed"))
        << QJsonDocument(resp).toJson().toStdString();

    EXPECT_EQ(readFile(roadmap), before) << "a refused convert rewrote the file";
    EXPECT_EQ(storedFormatOf(root), formatBefore)
        << "a refused convert left the store's dialect changed";
}

// ----------------------------------------------------------------- INV-3 ----

TEST(RoadmapConvert, convertIsIdempotent) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seed(guard, tmp, gfmRoadmap());
    ASSERT_FALSE(root.isEmpty());
    const QString roadmap = root + QStringLiteral("/ROADMAP.md");
    ASSERT_TRUE(migrate(root));

    ASSERT_TRUE(convert(root).value(QStringLiteral("ok")).toBool());
    const QByteArray once = hashOf(roadmap);

    // The second run's source is the first run's output, which is ants-v1 —
    // the dialect the convert must ACCEPT rather than refuse, or this is
    // unsatisfiable.
    const QJsonObject second = convert(root);
    ASSERT_TRUE(second.value(QStringLiteral("ok")).toBool())
        << "converting an already-converted project must not refuse: "
        << QJsonDocument(second).toJson().toStdString();
    EXPECT_EQ(hashOf(roadmap), once)
        << "a second convert changed the file — a bullet carrying a bracket id "
           "was allocated a second one";
}

// ----------------------------------------------------------------- INV-4 ----

TEST(RoadmapConvert, idsStableAcrossCommit) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seed(guard, tmp, gfmRoadmap());
    ASSERT_FALSE(root.isEmpty());
    ASSERT_TRUE(migrate(root));
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"),
                          gfmRoadmapPlusUnlaymanned()));

    const int countBefore = itemCountOf(root);
    ASSERT_GT(countBefore, 0);

    auto store = openStore();
    ASSERT_TRUE(store);
    QString err;
    const auto row = store->readProjectByRoot(root, &err);
    ASSERT_TRUE(row.has_value());
    const auto highBefore = store->idHighWater(row->projectId, QStringLiteral("PROJ"), &err);
    const auto synthBefore =
        store->idHighWater(row->projectId, QStringLiteral("PROJ#S"), &err);
    store.reset();

    // ANTS-5256 — the forced abort runs AFTER mutate() has allocated and BEFORE
    // the commit, which is exactly the window this invariant is about.
    // Asserted by CODE, so an earlier refusal cannot stand in for it and leave
    // this case green having exercised no allocation at all.
    QJsonObject aborted;
    {
        const ForcePostMutateFail forceFail;
        aborted = convert(root);
    }
    ASSERT_FALSE(aborted.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(aborted).toJson().toStdString();
    ASSERT_EQ(aborted.value(QStringLiteral("code")).toString().toStdString(),
              std::string("store_failed"))
        << QJsonDocument(aborted).toJson().toStdString();

    EXPECT_EQ(itemCountOf(root), countBefore)
        << "an aborted convert left rows behind";

    auto after = openStore();
    ASSERT_TRUE(after);
    const auto rowAfter = after->readProjectByRoot(root, &err);
    ASSERT_TRUE(rowAfter.has_value());
    EXPECT_EQ(after->idHighWater(rowAfter->projectId, QStringLiteral("PROJ"), &err)
                  .value_or(-1),
              highBefore.value_or(-1))
        << "an aborted convert burnt a real id";
    EXPECT_EQ(after->idHighWater(rowAfter->projectId, QStringLiteral("PROJ#S"), &err)
                  .value_or(-1),
              synthBefore.value_or(-1))
        << "an aborted convert burnt a synthesised id";
}

// ----------------------------------------------------------------- INV-5 ----

TEST(RoadmapConvert, existingIdsPreserved) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    // Mixed, which is the real input shape: op:"append" writes ants-v1 into a
    // GFM file, so any project that has filed work through the verb is mixed.
    const QByteArray mixed =
        gfmRoadmap()
        + QByteArray("- \xF0\x9F\x93\x8B [PROJ-0042] **An already-converted bullet.**\n"
                     "  Layman: A plain-language line.\n"
                     "  Kind: chore.\n"
                     "  Source: ants-4491-test.\n");
    const QString root = seed(guard, tmp, mixed);
    ASSERT_FALSE(root.isEmpty());
    const QString roadmap = root + QStringLiteral("/ROADMAP.md");
    ASSERT_TRUE(migrate(root));

    const QJsonObject resp = convert(root);
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();

    EXPECT_TRUE(readFile(roadmap).contains("[PROJ-0042]"))
        << "the convert did not preserve a bullet's existing bracket id";
}

// ----------------------------------------------------------------- INV-6 ----

TEST(RoadmapConvert, staleMirrorDoesNotDuplicate) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seed(guard, tmp, gfmRoadmap());
    ASSERT_FALSE(root.isEmpty());
    const QString roadmap = root + QStringLiteral("/ROADMAP.md");
    ASSERT_TRUE(migrate(root));
    const int countBefore = itemCountOf(root);
    ASSERT_GT(countBefore, 0);

    // The author files an id by hand for a bullet the store knows only by its
    // synthesised one. That id is live in the file and absent from the store —
    // the stale-mirror case ANTS-4500 § 4.5's re-match fallback covers.
    QByteArray withId = readFile(roadmap);
    withId.replace("- [ ] The first checklist bullet.",
                   "- \xF0\x9F\x93\x8B [PROJ-0900] **The first checklist bullet.**");
    ASSERT_TRUE(writeFile(roadmap, withId));

    const QJsonObject resp = convert(root);
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();

    EXPECT_EQ(itemCountOf(root), countBefore)
        << "an id live in the file and absent from the store produced a second "
           "item instead of matching the one it names";
    EXPECT_TRUE(readFile(roadmap).contains("[PROJ-0900]"))
        << "the author's hand-written id did not survive the republish";
}

// ---------------------------------------------------------------- INV-15 ----

TEST(RoadmapConvert, orphansRefuseInsteadOfResurrecting) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seed(guard, tmp, gfmRoadmap());
    ASSERT_FALSE(root.isEmpty());
    const QString roadmap = root + QStringLiteral("/ROADMAP.md");
    ASSERT_TRUE(migrate(root));

    // The author deletes a bullet the store already holds. The store keeps the
    // row — an orphan — which is Vestige's stale-snapshot case in miniature.
    QByteArray fewer = readFile(roadmap);
    const QByteArray gone = "- [ ] The second checklist bullet.\n"
                            "  Layman: A plain-language line.\n"
                            "  Kind: chore.\n"
                            "  Source: ants-4491-test.\n";
    ASSERT_TRUE(fewer.contains(gone));
    fewer.replace(gone, QByteArray());
    ASSERT_TRUE(writeFile(roadmap, fewer));
    const QByteArray hashBefore = hashOf(roadmap);

    for (const bool dryRun : {true, false}) {
        const QJsonObject resp = convert(root, dryRun);
        const std::string dump = QJsonDocument(resp).toJson().toStdString();
        EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool())
            << (dryRun ? "dry run" : "real run")
            << " converted a file with orphans, which republishes them:\n" << dump;
        EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
                  QStringLiteral("orphans_present")) << dump;
        EXPECT_EQ(resp.value(QStringLiteral("items_orphaned")).toInt(), 1) << dump;
        EXPECT_EQ(resp.value(QStringLiteral("orphaned_ids")).toArray().size(), 1)
            << dump;
    }
    EXPECT_EQ(hashOf(roadmap), hashBefore) << "a refused convert changed the file";
    EXPECT_EQ(storedFormatOf(root), QStringLiteral("github-task-list"))
        << "a refused convert changed the stored format";
}

// ----------------------------------------------------------------- INV-7 ----

TEST(RoadmapConvert, thirdDialectRefused) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seed(guard, tmp, passHeadingsRoadmap());
    ASSERT_FALSE(root.isEmpty());
    const QString roadmap = root + QStringLiteral("/ROADMAP.md");
    ASSERT_EQ(detectedFormatOf(roadmap), QStringLiteral("pass-headings"))
        << "setup: the fixture must classify as the third dialect, or this case "
           "tests the accepted path instead";
    ASSERT_TRUE(migrate(root));

    const QByteArray before = readFile(roadmap);
    const QJsonObject resp  = convert(root);
    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString().toStdString(),
              std::string("dialect_out_of_scope"))
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_EQ(readFile(roadmap), before) << "a refused convert rewrote the file";
}

// ----------------------------------------------------------------- INV-8 ----

// ANTS-5256 — a convert is a MIGRATION, not authoring, so the render's INV-5
// Layman gate reports on it instead of refusing it.
//
// Measured on Vestige 2026-09-21: 460 open items with no Layman line, so the op
// refused outright on the one project it was built for. ANTS-4628 narrowed that
// gate to the items a write TOUCHES precisely to unblock conversion — but a
// convert touches every item by construction, and ANTS-4500 gives every id-less
// bullet a synthesised id, making it an INSERT the store has never seen. So the
// narrowing cannot reach this and the gate that was relaxed to permit the
// conversion was the thing refusing it.
//
// The fixture mirrors failedConvertIsInert's deliberately: migrate the fully
// laymanned roadmap, THEN add an unlaymanned bullet, so the convert has to
// INSERT it. A bullet the store already holds re-matches, is not written, and
// never enters the gate's scope — so seeding it from the start would exercise
// nothing.
TEST(RoadmapConvert, laymanGateIsAdvisoryForConvert) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seed(guard, tmp, gfmRoadmap());
    ASSERT_FALSE(root.isEmpty());
    const QString roadmap = root + QStringLiteral("/ROADMAP.md");
    ASSERT_TRUE(migrate(root));
    ASSERT_TRUE(writeFile(roadmap, gfmRoadmapPlusUnlaymanned()));

    const QJsonObject resp = convert(root);
    // The whole point: this exact fixture is the one failedConvertIsInert used
    // to force render_gate_unmet with. It must now SUCCEED.
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << "the Layman gate still refuses a convert: "
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_EQ(detectedFormatOf(roadmap), QStringLiteral("ants-v1"))
        << "the convert reported success without republishing the file";

    // Reported, not waved through. The exemption relaxes a rule, so the
    // envelope has to say what it let past — otherwise a caller cannot tell an
    // exemption that found nothing from one that found 460.
    const QJsonObject missing =
        resp.value(QStringLiteral("layman_missing")).toObject();
    ASSERT_FALSE(missing.isEmpty())
        << "layman_missing absent — a silent exemption: "
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_EQ(missing.value(QStringLiteral("count")).toInt(), 1)
        << "expected exactly the one unlaymanned bullet: "
        << QJsonDocument(resp).toJson().toStdString();
    // Named, not just counted. A count alone cannot be checked against the file.
    const QJsonArray ids = missing.value(QStringLiteral("ids")).toArray();
    ASSERT_EQ(ids.size(), 1) << QJsonDocument(resp).toJson().toStdString();
    EXPECT_FALSE(ids.at(0).toString().isEmpty())
        << "an offender was counted but not named";

    // The exemption ends with the convert. An ORDINARY write touching that same
    // item must still be refused, or this has quietly disabled INV-5 for the
    // project rather than for the migration.
    const QString offender = ids.at(0).toString();
    QJsonObject flip;
    flip[QStringLiteral("caller_cwd")] = root;
    flip[QStringLiteral("op")]         = QStringLiteral("flip");
    flip[QStringLiteral("id")]         = offender;
    flip[QStringLiteral("to_status")]  = QStringLiteral("in-progress");
    RemoteControl rc(nullptr);
    const QJsonObject after = rc.cmdRoadmapLogFlipForTest(flip).object();
    EXPECT_FALSE(after.value(QStringLiteral("ok")).toBool())
        << "the Layman gate stayed disabled after the convert: "
        << QJsonDocument(after).toJson().toStdString();
    EXPECT_EQ(after.value(QStringLiteral("code")).toString().toStdString(),
              std::string("render_gate_unmet"))
        << QJsonDocument(after).toJson().toStdString();
}

// ----------------------------------------------------------------- INV-9 ----

// ANTS-5252 — the convert reports its id assignment PER BULLET, not only in
// aggregate, and a dry run is where that report has to be right.
//
// Vestige's case, which is better than the aggregate's: the op is a one-way
// bulk rewrite of a version-controlled PUBLIC file, and it moves a counter that
// CHANGELOG entries, specs and commit bodies cite by id. An aggregate count
// cannot be checked against anything; a per-bullet list can be read against the
// file. The asymmetry is what makes `in_file` the column to scan — a newly
// assigned id is visible and fixable, a CHANGED one is invisible and permanent.
//
// The fixture's bullets carry no bracket ids, so every row must be an owed
// ALLOCATION: origin "allocated", in_file false. That is exactly the population
// a reviewer is being asked to approve.
TEST(RoadmapConvert, dryRunReportsIdOriginPerBullet) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seed(guard, tmp, gfmRoadmap());
    ASSERT_FALSE(root.isEmpty());
    const QString roadmap = root + QStringLiteral("/ROADMAP.md");
    ASSERT_TRUE(migrate(root));
    const QByteArray before = readFile(roadmap);

    const QJsonObject resp = convert(root, /*dryRun=*/true);
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    // A preview writes nothing — the whole value of the report is that it is
    // readable BEFORE the rewrite.
    EXPECT_EQ(readFile(roadmap), before) << "a dry run rewrote the file";

    const QJsonObject ids = resp.value(QStringLiteral("ids")).toObject();
    const QJsonArray planned = ids.value(QStringLiteral("planned")).toArray();
    ASSERT_FALSE(planned.isEmpty())
        << "ids.planned absent — the aggregate is all a reviewer gets: "
        << QJsonDocument(resp).toJson().toStdString();
    // One row per bullet, not a sample. The fixture has three.
    EXPECT_EQ(planned.size(), ids.value(QStringLiteral("bullets_total")).toInt())
        << "the report does not cover every bullet: "
        << QJsonDocument(resp).toJson().toStdString();

    for (const auto v : planned) {
        const QJsonObject row = v.toObject();
        EXPECT_TRUE(row.contains(QStringLiteral("origin")))
            << "a row with no origin cannot be reviewed";
        EXPECT_EQ(row.value(QStringLiteral("origin")).toString().toStdString(),
                  std::string("absent"))
            << "an id-less bullet must report its id as absent from the file: "
            << QJsonDocument(row).toJson().toStdString();
        // The identity question. None of these ids is in the file yet.
        EXPECT_FALSE(row.value(QStringLiteral("in_file")).toBool())
            << "an id the convert is about to invent reported as already in "
               "the file — the one thing a reviewer is scanning for: "
            << QJsonDocument(row).toJson().toStdString();
        EXPECT_GT(row.value(QStringLiteral("line")).toInt(), 0)
            << "a row with no source line cannot be read against the file";
    }
}

// ANTS-5252 — a bullet that already carries a bracket id reports as `parsed`
// and in_file, so the two populations are distinguishable in the report. This
// is the half that makes the report worth reading: a reviewer is checking that
// the ids they already have did NOT change.
TEST(RoadmapConvert, reportDistinguishesExistingIdsFromAllocated) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    // One bullet with an explicit bracket id, one without.
    const QByteArray mixed =
        QByteArray("# Demo Roadmap\n"
                   "\n"
                   "## To Do\n"
                   "\n"
                   "- [ ] [PROJ-0042] An already-identified bullet.\n"
                   "  Layman: A plain-language line.\n"
                   "  Kind: chore.\n"
                   "  Source: ants-5252-test.\n"
                   "- [ ] A bullet with no id at all.\n"
                   "  Layman: A plain-language line.\n"
                   "  Kind: chore.\n"
                   "  Source: ants-5252-test.\n");
    const QString root = seed(guard, tmp, mixed);
    ASSERT_FALSE(root.isEmpty());
    ASSERT_TRUE(migrate(root));

    const QJsonObject resp = convert(root, /*dryRun=*/true);
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    const QJsonArray planned = resp.value(QStringLiteral("ids")).toObject()
                                   .value(QStringLiteral("planned")).toArray();
    ASSERT_EQ(planned.size(), 2) << QJsonDocument(resp).toJson().toStdString();

    int inFile = 0, allocated = 0;
    for (const auto v : planned) {
        const QJsonObject row = v.toObject();
        if (row.value(QStringLiteral("in_file")).toBool()) {
            ++inFile;
            EXPECT_EQ(row.value(QStringLiteral("id")).toString().toStdString(),
                      std::string("PROJ-0042"))
                << "the existing id was not reported back verbatim — which is "
                   "the change-of-identity case: "
                << QJsonDocument(row).toJson().toStdString();
        }
        if (row.value(QStringLiteral("origin")).toString()
            == QLatin1String("absent"))
            ++allocated;
    }
    EXPECT_EQ(inFile, 1) << "the existing id was not reported as already in the file: "
                         << QJsonDocument(resp).toJson().toStdString();
    EXPECT_EQ(allocated, 1) << "the id-less bullet was not reported as absent "
                               "from the file: "
                            << QJsonDocument(resp).toJson().toStdString();
}

// ---------------------------------------------------------------- INV-11 ----

// ANTS-5258 — an id-less bullet the load MATCHES to an existing store row
// reports that match, and names the id it claimed.
//
// This is the arm that needed previewing, and the reason inverts the obvious
// reading: a freshly allocated id collides with nothing, while a match writes
// an EXISTING id into the file. If it is the wrong row, every prior citation of
// that id now resolves to the wrong work — and the output is well-formed either
// way, so it cannot be reviewed after the fact.
//
// The fixture is the stale-mirror shape: migrate first, which allocates a
// migration-provenance id to each id-less bullet, then convert. The convert
// re-reads the FILE, which still carries no ids, so every plan row is `absent`
// — and the load then matches each to the row the migration made.
TEST(RoadmapConvert, reportNamesMatchedRows) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seed(guard, tmp, gfmRoadmap());
    ASSERT_FALSE(root.isEmpty());
    ASSERT_TRUE(migrate(root));

    const QJsonObject resp = convert(root, /*dryRun=*/true);
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    const QJsonObject ids = resp.value(QStringLiteral("ids")).toObject();

    // The triage counter, computed over every row rather than the capped echo.
    EXPECT_GT(ids.value(QStringLiteral("matched")).toInt(), 0)
        << "no row reported as matched, yet every bullet was migrated first — "
           "the load's outcome is not reaching the report: "
        << QJsonDocument(resp).toJson().toStdString();
    // Nothing here has a duplicate headline, so order-pairing must not fire.
    EXPECT_EQ(ids.value(QStringLiteral("ambiguous_rematch")).toInt(), 0)
        << QJsonDocument(resp).toJson().toStdString();

    const QJsonArray planned = ids.value(QStringLiteral("planned")).toArray();
    ASSERT_FALSE(planned.isEmpty());
    int matchedRows = 0;
    for (const auto v : planned) {
        const QJsonObject row = v.toObject();
        if (!row.value(QStringLiteral("matched")).toBool())
            continue;
        ++matchedRows;
        // Naming the id is the whole point — a bare `matched:true` tells a
        // reviewer an existing id was claimed and not WHICH, which is the one
        // thing they need in order to check it.
        EXPECT_FALSE(row.value(QStringLiteral("matched_id")).toString().isEmpty())
            << "matched with no id named: "
            << QJsonDocument(row).toJson().toStdString();
        EXPECT_FALSE(
            row.value(QStringLiteral("matched_headline")).toString().isEmpty())
            << "matched with no headline named — the reviewer cannot see what "
               "it matched ON: "
            << QJsonDocument(row).toJson().toStdString();
    }
    EXPECT_EQ(matchedRows, ids.value(QStringLiteral("matched")).toInt())
        << "the summary count and the rows disagree";
}

// ---------------------------------------------------------------- INV-12 ----

// ANTS-5258 — where several stored rows satisfy the match key, § 2.6.1 pairs
// them BY ORDER, and the report says so per bullet.
//
// The pairing is reproducible and the code is candid that it "rests on order
// alone". It has always fired an `ambiguous_rematch` NOTE, but a load note
// carries no line and cannot be correlated back to a bullet — so a caller could
// learn that something was paired by order and never which. That is the one arm
// a human should actually check, and it was the least reachable.
TEST(RoadmapConvert, ambiguousRematchIsReportedPerBullet) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    // Two byte-identical headlines in ONE section: the key is same-section +
    // migration-allocated + byte-identical headline, so this is the minimum
    // that makes a group of two.
    const QByteArray twins =
        QByteArray("# Demo Roadmap\n"
                   "\n"
                   "## To Do\n"
                   "\n"
                   "- [ ] A duplicated headline.\n"
                   "  Layman: A plain-language line.\n"
                   "  Kind: chore.\n"
                   "  Source: ants-5258-test.\n"
                   "- [ ] A duplicated headline.\n"
                   "  Layman: A plain-language line.\n"
                   "  Kind: chore.\n"
                   "  Source: ants-5258-test.\n");
    const QString root = seed(guard, tmp, twins);
    ASSERT_FALSE(root.isEmpty());
    ASSERT_TRUE(migrate(root));

    const QJsonObject resp = convert(root, /*dryRun=*/true);
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    const QJsonObject ids = resp.value(QStringLiteral("ids")).toObject();

    EXPECT_GT(ids.value(QStringLiteral("ambiguous_rematch")).toInt(), 0)
        << "two identical headlines in one section were paired by order and "
           "the report did not say so: "
        << QJsonDocument(resp).toJson().toStdString();

    const QJsonArray planned = ids.value(QStringLiteral("planned")).toArray();
    int flagged = 0;
    for (const auto v : planned)
        if (v.toObject().value(QStringLiteral("ambiguous_rematch")).toBool())
            ++flagged;
    EXPECT_GT(flagged, 0)
        << "the summary counted an ambiguous pairing but no ROW carries the "
           "flag — a caller cannot tell which bullet to check: "
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_EQ(flagged, ids.value(QStringLiteral("ambiguous_rematch")).toInt())
        << "the summary count and the flagged rows disagree";
}

// ---------------------------------------------------------------- INV-13 ----

// ANTS-5260 — a convert preserves the PROSE, not only the bullets.
//
// Vestige measured drift_lost 2840 on their project and the lost text opened
// with their entire "How this file is organised" preamble — the narration a
// future session reads to learn how to file into that roadmap at all. That
// figure is check_sync against the STALE store and does not predict what a
// convert does, which is why this asks the question directly instead.
//
// Three placements, because they are carried by different machinery: prose
// ABOVE the first heading (the preamble, which belongs to a synthetic section
// with an empty slug), prose BETWEEN a heading and its first bullet (a section
// intro), and prose AFTER the last bullet. A convert that keeps one and drops
// another would pass a laxer test and still lose the file's instructions.
TEST(RoadmapConvert, convertPreservesNarration) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QByteArray withProse =
        QByteArray("# Demo Roadmap\n"
                   "\n"
                   "PREAMBLE-SENTINEL: how this file is organised, and how to\n"
                   "file into it.\n"
                   "\n"
                   "## To Do\n"
                   "\n"
                   "INTRO-SENTINEL: what this section is for.\n"
                   "\n"
                   "- [ ] The first checklist bullet.\n"
                   "  Layman: A plain-language line.\n"
                   "  Kind: chore.\n"
                   "  Source: ants-5260-test.\n"
                   "\n"
                   "TAIL-SENTINEL: a closing note after the last bullet.\n");

    const QString root = seed(guard, tmp, withProse);
    ASSERT_FALSE(root.isEmpty());
    const QString roadmap = root + QStringLiteral("/ROADMAP.md");
    ASSERT_TRUE(migrate(root));

    const QJsonObject resp = convert(root);
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();

    const QByteArray after = readFile(roadmap);
    ASSERT_FALSE(after.isEmpty());
    // Each named separately: "some prose survived" is the assertion that would
    // pass while the file's filing instructions were dropped.
    EXPECT_TRUE(after.contains("PREAMBLE-SENTINEL"))
        << "the preamble above the first heading did not survive the convert — "
           "this is the text a future session reads to learn how to file:\n"
        << after.toStdString();
    EXPECT_TRUE(after.contains("INTRO-SENTINEL"))
        << "the section intro did not survive the convert:\n"
        << after.toStdString();
    EXPECT_TRUE(after.contains("TAIL-SENTINEL"))
        << "prose after the last bullet did not survive the convert:\n"
        << after.toStdString();
}

// ---------------------------------------------------------------- INV-14 ----

// ANTS-5260 — prose BETWEEN bullets keeps its POSITION, not merely its bytes.
//
// INV-13 asserts presence and says so honestly. This is the assertion it does
// not make, and Vestige measured that it is the shape their file is full of:
// 41 distinct runs of mid-section prose, of which 8 are BOLD PSEUDO-HEADINGS —
// bold lines that function as headings and are not headings, because the
// markdown has no heading level left (their innermost are already h6).
//
//     **Long-deferred (acknowledged-but-not-this-cycle):**
//     **Cross-doc / cross-phase decisions that need an owner sign-off:**
//
// Each labels the bullets BELOW it and its meaning is ENTIRELY positional. A
// convert that preserves every such line and re-files it anywhere but
// immediately above its own bullets passes INV-13 and silently regroups the
// roadmap — and "long-deferred" versus "needs sign-off" is the difference
// between an item being parked and being scheduled. That is a semantic loss
// wearing a clean diff.
//
// The horizontal rule is here for a different reason: it is structure with NO
// content. If narration is captured as text-bearing lines only, a `---` is
// dropped with nothing semantically lost and a visible diff produced — which
// would then read as a defect in someone's before/after review when it is not.
TEST(RoadmapConvert, midSectionProseKeepsItsPosition) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QByteArray interleaved =
        QByteArray("# Demo Roadmap\n"
                   "\n"
                   "## To Do\n"
                   "\n"
                   "- [ ] The bullet ABOVE the pseudo-heading.\n"
                   "  Layman: A plain-language line.\n"
                   "  Kind: chore.\n"
                   "  Source: ants-5260-test.\n"
                   "\n"
                   "---\n"
                   "\n"
                   "**PSEUDOHEADING-SENTINEL:**\n"
                   "\n"
                   "- [ ] The bullet BELOW the pseudo-heading.\n"
                   "  Layman: A plain-language line.\n"
                   "  Kind: chore.\n"
                   "  Source: ants-5260-test.\n");

    const QString root = seed(guard, tmp, interleaved);
    ASSERT_FALSE(root.isEmpty());
    const QString roadmap = root + QStringLiteral("/ROADMAP.md");
    ASSERT_TRUE(migrate(root));

    const QJsonObject resp = convert(root);
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();

    const QString after = QString::fromUtf8(readFile(roadmap));
    ASSERT_FALSE(after.isEmpty());

    const int above  = after.indexOf(QStringLiteral("ABOVE the pseudo-heading"));
    const int label  = after.indexOf(QStringLiteral("PSEUDOHEADING-SENTINEL"));
    const int below  = after.indexOf(QStringLiteral("BELOW the pseudo-heading"));

    // Presence first, so a failure says WHICH thing went missing rather than
    // failing an ordering comparison against -1.
    ASSERT_GE(above, 0) << "the first bullet did not survive:\n" << after.toStdString();
    ASSERT_GE(label, 0)
        << "the bold pseudo-heading did not survive — 8 of these carry the "
           "grouping semantics of a real roadmap:\n"
        << after.toStdString();
    ASSERT_GE(below, 0) << "the second bullet did not survive:\n" << after.toStdString();

    // The assertion INV-13 does not make. Carried is not the same as carried
    // in place, and only position preserves what a pseudo-heading MEANS.
    EXPECT_LT(above, label)
        << "the pseudo-heading moved ABOVE the bullet it followed — it now "
           "labels the wrong group:\n"
        << after.toStdString();
    EXPECT_LT(label, below)
        << "the pseudo-heading no longer sits immediately above the bullets it "
           "labels — every line survived and the grouping is silently wrong, "
           "which is a clean diff hiding a semantic loss:\n"
        << after.toStdString();

    // Structure with no content. Reported rather than asserted either way:
    // dropping it loses no meaning, but it shows up in a before/after diff and
    // a reviewer needs to know whether that is expected.
    if (!after.contains(QStringLiteral("\n---\n"))) {
        GTEST_LOG_(INFO) << "note: the horizontal rule was not reproduced — "
                            "no semantic loss, but it will appear in a "
                            "before/after diff and is not a defect";
    }
}


// ---------------------------------------------------------------- INV-16 ----

// ANTS-5286 — a real convert that would drop text refuses unless the caller
// accepts the loss. What shipped first was a warning, and a session followed
// it through by default on evidence too large to read. No fixture tried makes
// the real convert lose text, so the seam stands in for a lossy render.
TEST(RoadmapConvert, textLossRefusesUnlessAccepted) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seed(guard, tmp, gfmRoadmap());
    ASSERT_FALSE(root.isEmpty());
    const QString roadmap = root + QStringLiteral("/ROADMAP.md");
    ASSERT_TRUE(migrate(root));
    const QByteArray hashBefore = hashOf(roadmap);

    RoadmapWrite::setDropFromPostImageForTest(QStringLiteral("second checklist"));
    struct ClearSeam {
        ~ClearSeam() { RoadmapWrite::setDropFromPostImageForTest(QString()); }
    } clearSeam;

    // A dry run reports; it never refuses.
    const QJsonObject dry = convert(root, /*dryRun=*/true);
    EXPECT_TRUE(dry.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(dry).toJson().toStdString();
    EXPECT_EQ(dry.value(QStringLiteral("would_discard_reason")).toString(),
              QStringLiteral("text_lost"));

    // A real run refuses, names the line, and writes nothing.
    const QJsonObject refused = convert(root);
    const std::string dump = QJsonDocument(refused).toJson().toStdString();
    EXPECT_FALSE(refused.value(QStringLiteral("ok")).toBool()) << dump;
    EXPECT_EQ(refused.value(QStringLiteral("code")).toString(),
              QStringLiteral("text_lost")) << dump;
    EXPECT_EQ(refused.value(QStringLiteral("discarded_text_lines")).toInt(), 1) << dump;
    const QJsonArray named = refused.value(QStringLiteral("discarded_text")).toArray();
    ASSERT_EQ(named.size(), 1) << dump;
    EXPECT_TRUE(named.at(0).toString().contains(QStringLiteral("second checklist")))
        << dump;
    EXPECT_EQ(hashOf(roadmap), hashBefore) << "a refused convert changed the file";
    EXPECT_EQ(storedFormatOf(root), QStringLiteral("github-task-list"))
        << "a refused convert changed the stored format";

    // accept_text_loss:true proceeds.
    QJsonObject req;
    req[QStringLiteral("caller_cwd")]       = root;
    req[QStringLiteral("op")]               = QStringLiteral("convert");
    req[QStringLiteral("accept_text_loss")] = true;
    RemoteControl rc(nullptr);
    const QJsonObject accepted = rc.cmdRoadmapLogConvertForTest(req).object();
    EXPECT_TRUE(accepted.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(accepted).toJson().toStdString();
    EXPECT_EQ(storedFormatOf(root), QStringLiteral("ants-v1"));
}

// ---------------------------------------------------------------- INV-17 ----

// ANTS-5330 — the top-level `layman_missing` and the per-row flags count the
// same population: OPEN items with no Layman. Vestige saw count:0 beside 164
// flagged rows, because the count took only items the convert WROTE and a
// re-matched item is not rewritten, while the rows flagged closed items too.
// Migrating first makes the convert re-match every bullet, which is that case.
TEST(RoadmapConvert, laymanMissingCountAgreesWithRows) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seed(guard, tmp, gfmRoadmapPlusUnlaymanned()
        + QByteArray("- [x] A shipped bullet with no Layman line.\n"
                     "  Kind: chore.\n"
                     "  Source: ants-4491-test.\n"));
    ASSERT_FALSE(root.isEmpty());
    ASSERT_TRUE(migrate(root));

    const QJsonObject resp = convert(root, /*dryRun=*/true);
    const std::string dump = QJsonDocument(resp).toJson().toStdString();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool()) << dump;
    const int count = resp.value(QStringLiteral("layman_missing")).toObject()
                          .value(QStringLiteral("count")).toInt();
    int flagged = 0;
    const QJsonArray planned = resp.value(QStringLiteral("ids")).toObject()
                                   .value(QStringLiteral("planned")).toArray();
    for (const QJsonValue &v : planned)
        if (v.toObject().value(QStringLiteral("layman_missing")).toBool())
            ++flagged;
    EXPECT_EQ(count, 1) << "the one OPEN bullet without Layman is owed one:\n" << dump;
    EXPECT_EQ(flagged, 1) << "a shipped bullet owes no Layman and is not flagged:\n"
                          << dump;
}
