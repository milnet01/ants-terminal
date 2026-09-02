// Feature-conformance test for ANTS-4402 — the roadmap source witness.
// Contract: tests/features/roadmap_source_witness/spec.md
//
// Behavioural through RemoteControl::cmdRoadmapQuery against real fixture
// trees. XDG_DATA_HOME is redirected per case so RoadmapStore::defaultPath()
// — which cmdRoadmapQuery resolves internally and no argument overrides —
// lands in a QTemporaryDir instead of the developer's REAL store.

#include "remotecontrol.h"
#include "roadmaprender.h"
#include "roadmapmigrate.h"
#include "roadmapmigrateload.h"
#include "roadmapstore.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

namespace {

bool writeFile(const QString &path, const QByteArray &text) {
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return f.write(text) == text.size();
}

// An ants-v1 roadmap: the dialect gate in RoadmapSource::migratedProject()
// serves the store only for this format, so a fixture without the marker would
// silently exercise the markdown branch in every case.
QByteArray roadmapText() {
    return "<!-- ants-roadmap-format: 1 -->\n"
           "\n"
           "# Demo — Roadmap\n"
           "\n"
           "## Work\n"
           "\n"
           "- \xF0\x9F\x93\x8B [DEMO-0001] **An open item.**\n"
           "  Layman: A thing.\n"
           "  Kind: implement.\n";
}

// The bullet a session files BY HAND under ANTS-4141's workaround: a real id,
// far above anything the migration imported, written straight into the file.
QByteArray handFiledBullet() {
    return "\n- \xF0\x9F\x93\x8B [DEMO-4242] **Filed by hand, after the migration.**\n"
           "  Layman: A thing.\n"
           "  Kind: implement.\n";
}

// Redirects XDG_DATA_HOME for its lifetime and restores the prior value —
// including its ABSENCE, which is a different state from empty: leaving an
// empty XDG_DATA_HOME set would point every later test in this binary at the
// filesystem root rather than at the user's real data dir.
class XdgRedirect {
public:
    explicit XdgRedirect(const QString &dir)
        : m_had(qEnvironmentVariableIsSet("XDG_DATA_HOME")),
          m_prior(m_had ? qgetenv("XDG_DATA_HOME") : QByteArray()) {
        qputenv("XDG_DATA_HOME", dir.toLocal8Bit());
    }
    ~XdgRedirect() {
        if (m_had)
            qputenv("XDG_DATA_HOME", m_prior);
        else
            qunsetenv("XDG_DATA_HOME");
    }
    XdgRedirect(const XdgRedirect &) = delete;
    XdgRedirect &operator=(const XdgRedirect &) = delete;

private:
    bool m_had;
    QByteArray m_prior;
};

// findRoadmaps → planFrom → load, the migration as a consumer runs it. Bulk,
// because RoadmapMigrateLoad::load() refuses an Interactive connection
// (ANTS-3765 INV-12).
bool migrateDefaultStore(const QString &root) {
    const QString dbPath = RoadmapStore::defaultPath();
    QDir().mkpath(QFileInfo(dbPath).path());
    RoadmapStore store(dbPath, RoadmapStore::kDefaultHistoryCapBytes,
                       RoadmapStore::Access::Bulk);
    QString err;
    if (!store.open(&err)) {
        ADD_FAILURE() << "store open: " << err.toStdString();
        return false;
    }
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    if (!disc) {
        ADD_FAILURE() << "findRoadmaps: " << err.toStdString();
        return false;
    }
    const auto plan =
        RoadmapMigrate::planFrom(*disc, QStringLiteral("Demo"),
                                 QStringLiteral("demo"));
    RoadmapMigrateLoad::Options opts;
    opts.changedAt = QStringLiteral("2026-08-15T10:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(store, plan, opts);
    if (!out.ok) {
        ADD_FAILURE() << "migration load: " << out.error.toStdString();
        return false;
    }
    return true;
}

// ANTS-4462 — PUBLISH the store over the file, so the file becomes the render's
// own output. Without this the fixture is hand-written markdown the render has
// never touched, and it differs from the store's canonical form by dialect
// alone — real drift, but not the drift a staleness case is about. Rendering
// first removes that confound so the flip below is the ONLY difference.
bool renderStore(const QString &root) {
    RoadmapStore store(RoadmapStore::defaultPath(),
                       RoadmapStore::kDefaultHistoryCapBytes,
                       RoadmapStore::Access::Interactive);
    QString err;
    if (!store.open(&err)) {
        ADD_FAILURE() << "store open: " << err.toStdString();
        return false;
    }
    const auto pid = store.projectIdForRoot(root, &err);
    if (!pid) {
        ADD_FAILURE() << "projectIdForRoot: " << err.toStdString();
        return false;
    }
    RoadmapRender::Options opts;
    opts.liveRoadmapPath = root + QStringLiteral("/ROADMAP.md");
    const auto out = RoadmapRender::render(store, *pid, root, opts, &err);
    if (!out || !out->committed) {
        ADD_FAILURE() << "render: " << err.toStdString();
        return false;
    }
    return true;
}

// A FRESH RemoteControl per query: cmdRoadmapQuery memoises bullets on
// (path, mtime) with a 100 ms TTL, and mtime has 1-second resolution on some
// filesystems — so a case that rewrites the fixture and reuses one instance
// can read its own stale cache and pass for the wrong reason.
QJsonObject query(const QString &root) {
    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    return rc.cmdRoadmapQuery(req).object();
}

// ANTS-4462 — one query with the staleness check engaged. A FRESH
// RemoteControl for the same reason query() takes one.
QJsonObject checkSync(const QString &root) {
    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("check_sync")] = true;
    return rc.cmdRoadmapQuery(req).object();
}

}  // namespace

// ---------------------------------------------------------------- INV-1 -----

TEST(RoadmapSourceWitness, Inv1MarkdownSource) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const XdgRedirect xdg(dir.filePath(QStringLiteral("xdg")));

    const QString root = dir.filePath(QStringLiteral("proj"));
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"), roadmapText()));

    const QJsonObject out = query(root);
    ASSERT_TRUE(out.value(QStringLiteral("ok")).toBool()) << "query refused";
    EXPECT_EQ(out.value(QStringLiteral("source")).toString(),
              QStringLiteral("markdown"))
        << "no store on this machine — the file IS the answer";
    // The markdown backend cannot be stale against itself, so none of the
    // warning triple may appear.
    EXPECT_FALSE(out.contains(QStringLiteral("file_ahead_of_store")));
    EXPECT_FALSE(out.contains(QStringLiteral("file_highest_id")));
    EXPECT_FALSE(out.contains(QStringLiteral("store_high_water")));
}

// ---------------------------------------------------------------- INV-2 -----

TEST(RoadmapSourceWitness, Inv2StoreSource) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const XdgRedirect xdg(dir.filePath(QStringLiteral("xdg")));

    const QString root = dir.filePath(QStringLiteral("proj"));
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"), roadmapText()));
    ASSERT_TRUE(migrateDefaultStore(root));

    const QJsonObject out = query(root);
    ASSERT_TRUE(out.value(QStringLiteral("ok")).toBool()) << "query refused";
    EXPECT_EQ(out.value(QStringLiteral("source")).toString(),
              QStringLiteral("store"))
        << "a migrated project is served by the store, and must say so — this "
           "field sits beside a `path` naming a file the answer did not come "
           "from, which is the whole defect";
}

// ---------------------------------------------------------------- INV-3 -----

TEST(RoadmapSourceWitness, Inv3FileAheadOfStoreWarns) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const XdgRedirect xdg(dir.filePath(QStringLiteral("xdg")));

    const QString root = dir.filePath(QStringLiteral("proj"));
    const QString rmPath = root + QStringLiteral("/ROADMAP.md");
    ASSERT_TRUE(writeFile(rmPath, roadmapText()));
    ASSERT_TRUE(migrateDefaultStore(root));

    // The ANTS-4141 workaround, performed literally: append a bullet to the
    // file the store has already been built from. Nothing re-imports it.
    ASSERT_TRUE(writeFile(rmPath, roadmapText() + handFiledBullet()));

    const QJsonObject out = query(root);
    ASSERT_TRUE(out.value(QStringLiteral("ok")).toBool()) << "query refused";
    EXPECT_EQ(out.value(QStringLiteral("source")).toString(),
              QStringLiteral("store"));
    EXPECT_TRUE(out.value(QStringLiteral("file_ahead_of_store")).toBool())
        << "DEMO-4242 is in the file and not in the store; a reader given "
           "ok:true and no warning has no way to learn that";
    EXPECT_EQ(out.value(QStringLiteral("file_highest_id")).toInt(), 4242);
    EXPECT_LT(out.value(QStringLiteral("store_high_water")).toInt(), 4242)
        << "both numbers are reported so the gap is visible, not inferred";
}

// ---------------------------------------------------------------- INV-4 -----

// ANTS-4462 — `check_sync` answers by CONTENT, in both directions, where
// `file_ahead_of_store` can only answer by ID and only in one.
//
// INV-3 above covers the case the id witness CAN see: a bullet filed by hand
// carrying an id above the store's mark. The reported defect is the case it
// cannot — a hand STATUS FLIP on a bullet the store already holds. No id moves,
// so `file_ahead_of_store` stays absent and the envelope reads healthy while
// the store serves the pre-flip status. Measured on the reporting project:
// two ✅ headlines returned as 📋, ok:true, no warning anywhere.
TEST(RoadmapSourceWitness, Inv4CheckSyncSeesAHandFlipTheIdWitnessCannot) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    XdgRedirect redirect(tmp.path());
    QDir dir(tmp.path());
    ASSERT_TRUE(dir.mkpath(QStringLiteral("proj")));
    const QString root = dir.filePath(QStringLiteral("proj"));
    const QString rmPath = root + QStringLiteral("/ROADMAP.md");
    ASSERT_TRUE(writeFile(rmPath, roadmapText()));
    ASSERT_TRUE(migrateDefaultStore(root));
    ASSERT_TRUE(renderStore(root));

    // The baseline, on the published file, before anything is edited.
    const QJsonObject before = checkSync(root);
    ASSERT_TRUE(before.value(QStringLiteral("ok")).toBool());

    // Flip 📋 -> ✅ in the FILE only, on the file the render just published.
    // Same bullet, same id, same everything else — exactly what a
    // `git checkout --` or a hand edit leaves behind.
    QFile pub(rmPath);
    ASSERT_TRUE(pub.open(QIODevice::ReadOnly));
    QByteArray flipped = pub.readAll();
    pub.close();
    const QByteArray published = flipped;
    flipped.replace("\xF0\x9F\x93\x8B [DEMO-0001]", "\xE2\x9C\x85 [DEMO-0001]");
    ASSERT_NE(flipped, published) << "the fixture flip must actually apply";
    ASSERT_TRUE(writeFile(rmPath, flipped));

    const QJsonObject after = checkSync(root);
    ASSERT_TRUE(after.value(QStringLiteral("ok")).toBool()) << "query refused";
    ASSERT_EQ(after.value(QStringLiteral("source")).toString(),
              QStringLiteral("store"))
        << "this case is only meaningful when the store is answering";

    // The gap, asserted as a CHANGE rather than as an absence. What matters
    // here is that the flip does not MOVE the id witness — a status flip moves
    // no id, so the id witness cannot be what detects one.
    //
    // ANTS-4636 — this used to say the witness "has a baseline value on this
    // fixture for reasons that have nothing to do with the flip", so asserting
    // absence would be asserting something else. That baseline value was the
    // defect ANTS-4633 filed and ANTS-4636 fixed: migration writes no id_prefix
    // row, so the witness read 0 against a file holding DEMO-0001. It is absent
    // now, and Inv6 below asserts that directly. The change-assertion stays
    // because it is a different statement, and still the one this case needs.
    EXPECT_EQ(before.value(QStringLiteral("file_ahead_of_store")),
              after.value(QStringLiteral("file_ahead_of_store")))
        << "the flip changed the id witness — then the premise of ANTS-4462's "
           "read half has changed and this test no longer covers its case";

    // The content check, on the same two calls, does move.
    EXPECT_TRUE(before.value(QStringLiteral("file_in_sync")).toBool())
        << "the render had just published this file";
    EXPECT_FALSE(after.value(QStringLiteral("file_in_sync")).toBool())
        << "the file says done and the store says open; that is not in sync";
    EXPECT_GT(after.value(QStringLiteral("drift_lines")).toInt(), 0);
    // ANTS-4730 — this asserted ABSENCE on a measurement that ran, which is
    // the shape perch reported as a defect. The schema tells a caller to
    // branch on sync_checked BEFORE trusting the answer, so absence on the
    // one arm that proves the check ran is read as "nobody looked" — by the
    // careful caller, not the careless one. The field is emitted on both arms
    // now, and absence means only that check_sync was not requested.
    EXPECT_TRUE(after.value(QStringLiteral("sync_checked")).toBool())
        << "the render-and-compare ran, so the field must say so";
}

// ANTS-4818 — mode:"report" answers check_sync instead of dropping it.
// ANTS-4730 makes an absent `sync_checked` mean "not requested" and nothing
// else, so a caller who DID request it and got no field read the absence as
// their own omission. Reported against this project by a session that tried
// report first, which is the natural place to ask a project-level question
// about the file.
TEST(RoadmapSourceWitness, Ants4818ReportModeAnswersCheckSync) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    XdgRedirect redirect(tmp.path());
    QDir dir(tmp.path());
    ASSERT_TRUE(dir.mkpath(QStringLiteral("proj")));
    const QString root = dir.filePath(QStringLiteral("proj"));
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"), roadmapText()));
    ASSERT_TRUE(migrateDefaultStore(root));
    ASSERT_TRUE(renderStore(root));

    RemoteControl rc(nullptr);
    auto report = [&](bool ask) {
        QJsonObject req;
        req[QStringLiteral("caller_cwd")] = root;
        req[QStringLiteral("mode")]       = QStringLiteral("report");
        if (ask) req[QStringLiteral("check_sync")] = true;
        return rc.cmdRoadmapQuery(req).object();
    };

    const QJsonObject asked = report(true);
    ASSERT_TRUE(asked.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(asked).toJson().toStdString();
    ASSERT_TRUE(asked.contains(QStringLiteral("sync_checked")))
        << "check_sync was requested, so the answer must be in the envelope";
    EXPECT_TRUE(asked.value(QStringLiteral("sync_checked")).toBool())
        << "the store had just been rendered to the file, so a measurement "
           "was available and must have been taken";
    EXPECT_TRUE(asked.value(QStringLiteral("file_in_sync")).toBool());

    // And absence still means exactly one thing, or the field above is not
    // the signal ANTS-4730 says it is.
    const QJsonObject unasked = report(false);
    ASSERT_TRUE(unasked.value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(unasked.contains(QStringLiteral("sync_checked")))
        << "an unrequested check must leave no field behind";
}

// ANTS-4462 — and it reports the healthy case as healthy, so the signal is
// worth something. A check that cried wolf on every project would be ignored.
TEST(RoadmapSourceWitness, Inv4CheckSyncIsQuietOnAnUneditedProject) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    XdgRedirect redirect(tmp.path());
    QDir dir(tmp.path());
    ASSERT_TRUE(dir.mkpath(QStringLiteral("proj")));
    const QString root = dir.filePath(QStringLiteral("proj"));
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"), roadmapText()));
    ASSERT_TRUE(migrateDefaultStore(root));
    ASSERT_TRUE(renderStore(root));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("check_sync")] = true;
    const QJsonObject out = rc.cmdRoadmapQuery(req).object();
    ASSERT_TRUE(out.value(QStringLiteral("ok")).toBool());
    ASSERT_EQ(out.value(QStringLiteral("source")).toString(),
              QStringLiteral("store"));

    EXPECT_TRUE(out.value(QStringLiteral("file_in_sync")).toBool())
        << "the render just published this file — it IS the store's output";
    // The drift counters ride the true arm only — zeros on every healthy
    // check are fields nobody reads.
    EXPECT_FALSE(out.contains(QStringLiteral("drift_lines")));
    // ANTS-4730 — sync_checked does NOT ride the true arm with the counters,
    // and that asymmetry is the point: the counters are detail a healthy
    // caller can skip, while this is the field the schema tells every caller
    // to read first. A field that is absent exactly when the news is good
    // cannot serve that purpose.
    EXPECT_TRUE(out.value(QStringLiteral("sync_checked")).toBool());
}

// ANTS-4710 — an unparseable roadmap and a sectionless one must not answer
// identically.
//
// Reported after measuring two projects. Against a git repo whose ROADMAP.md
// is prose — no headings, no bullets — mode:"section_index" answered total:0
// with no `slugs` and no `warning`; against a store-backed project it answers
// with its slugs. So the two cases a caller most needs to separate — "this
// file is not a roadmap" and "this roadmap has no sections" — differed only
// in a count that is 0 either way. A shared skill prescribes this one call as
// the whole answer to which roadmap is authoritative, so it reports "no
// sections" when the true answer is "unreadable".
TEST(RoadmapSourceWitness, Ants4710SectionIndexWarnsOnAnUnparseableRoadmap) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    XdgRedirect redirect(tmp.path());
    QDir dir(tmp.path());
    ASSERT_TRUE(dir.mkpath(QStringLiteral("proj")));
    const QString root = dir.filePath(QStringLiteral("proj"));
    // Prose: no headings, no bullets, nothing this verb can parse.
    ASSERT_TRUE(writeFile(
        root + QStringLiteral("/ROADMAP.md"),
        QByteArrayLiteral("We plan to ship the thing.\n"
                          "Then we plan to ship the next thing.\n")));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("mode")]       = QStringLiteral("section_index");
    const QJsonObject out = rc.cmdRoadmapQuery(req).object();

    ASSERT_TRUE(out.value(QStringLiteral("ok")).toBool())
        << "precondition: code="
        << out.value(QStringLiteral("code")).toString().toStdString();
    ASSERT_TRUE(out.value(QStringLiteral("sections")).toArray().isEmpty())
        << "precondition: this fixture has no parseable sections, which is "
           "the whole case — with sections there is nothing ambiguous to warn "
           "about";
    ASSERT_TRUE(out.contains(QStringLiteral("warning")))
        << "ANTS-4710: total:0 alone cannot tell an unreadable roadmap from a "
           "sectionless one";
    EXPECT_TRUE(out.value(QStringLiteral("warning"))
                    .toString()
                    .contains(QStringLiteral("format not recognised")))
        << "this fixture has neither headings nor id-bearing bullets, so the "
           "warning must name the UNREADABLE case; naming the sectionless one "
           "here would be a confident wrong answer rather than no answer";
}

// ANTS-4462 — an unmigrated project must not read as "in sync". Nobody looked
// is a different answer from clean, and conflating them is the ANTS-4463
// lesson in the other direction: a present field asserting an unchecked fact.
TEST(RoadmapSourceWitness, Inv4CheckSyncSaysNobodyLookedOnMarkdown) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    XdgRedirect redirect(tmp.path());
    QDir dir(tmp.path());
    ASSERT_TRUE(dir.mkpath(QStringLiteral("proj")));
    const QString root = dir.filePath(QStringLiteral("proj"));
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"), roadmapText()));
    // Deliberately NOT migrated.

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("check_sync")] = true;
    const QJsonObject out = rc.cmdRoadmapQuery(req).object();
    ASSERT_TRUE(out.value(QStringLiteral("ok")).toBool());
    ASSERT_EQ(out.value(QStringLiteral("source")).toString(),
              QStringLiteral("markdown"));

    EXPECT_FALSE(out.value(QStringLiteral("sync_checked")).toBool())
        << "no store to compare against — that is not a clean bill of health";
    EXPECT_FALSE(out.contains(QStringLiteral("file_in_sync")))
        << "a project with no store cannot be in or out of sync with one";
}

// ---------------------------------------------------------------- INV-6 -----

// ANTS-4636 — a freshly migrated project is NOT ahead of its own store.
//
// `idHighWater()` reads the `id_prefix` row, and roadmapstore.h says outright
// that migration does not write one: only an id-allocating append does. So a
// migrated-but-never-appended project reported a store high-water of 0 while
// holding every id in its file, and `file_highest_id > 0` pinned the warning on
// permanently. Album_Builder hit it with 357 items and read it as "the
// migration did not land"; this fixture hits it with one, which is the baseline
// firing ANTS-4633 filed and could not explain.
//
// Both reports guessed a prefix mismatch. The prefix is correct — maxDeclaredId
// filters strictly by it, so a wrong prefix would have zeroed `file_highest_id`
// too and fired nothing at all.
//
// A false staleness warning is worth a test of its own: the flag's whole value
// is that it is rare, and one that fires on every healthy migrated project is
// one people learn to ignore.
TEST(RoadmapSourceWitness, Inv6FreshlyMigratedProjectIsNotAheadOfItsStore) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const XdgRedirect xdg(dir.filePath(QStringLiteral("xdg")));

    const QString root = dir.filePath(QStringLiteral("proj"));
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"), roadmapText()));
    ASSERT_TRUE(migrateDefaultStore(root));
    // Deliberately NO append between the migration and the query: an append is
    // what writes the id_prefix row, so appending here would paper over the
    // exact condition this case exists to cover.

    const QJsonObject out = query(root);
    ASSERT_TRUE(out.value(QStringLiteral("ok")).toBool()) << "query refused";
    ASSERT_EQ(out.value(QStringLiteral("source")).toString(),
              QStringLiteral("store"))
        << "only meaningful when the store is answering";

    EXPECT_FALSE(out.contains(QStringLiteral("file_ahead_of_store")))
        << "the store holds every id the file declares — warning that the file "
           "is ahead tells a session its migration silently wrote nothing\n"
        << QJsonDocument(out).toJson().toStdString();
    EXPECT_GE(out.value(QStringLiteral("store_high_water")).toInt(),
              out.value(QStringLiteral("file_highest_id")).toInt())
        << "and the two numbers must agree, since they describe the same ids";
}
