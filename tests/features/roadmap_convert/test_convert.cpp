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
