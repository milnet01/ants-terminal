// Feature-conformance test for ANTS-4470 (op:"annotate_batch"), ANTS-4466 (the
// store path's from_status/to_status) and ANTS-4464 (write_path + the store
// path's post_bullets echo). See
// tests/features/roadmap_log_annotate_batch/spec.md.
//
// INV-6's case is the reported one and cannot be reached on the markdown path:
// it needs a store whose status DISAGREES with the file, which is what a
// `git checkout --` of ROADMAP.md leaves behind. So the store cases migrate a
// temp project, then rewrite the file underneath it.

#include "../../_support/xdg_guard.h"

#include "remotecontrol.h"
#include "roadmapmigrate.h"
#include "roadmapmigrateload.h"
#include "roadmapstore.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>

#include <memory>
#include <string>

namespace {

// ants-v1 seed, padded past kRoadmapMinParseableSize (1024 B) so the ants-v1
// walker fallback engages. 📋 = U+1F4CB.
QByteArray seedV1() {
    return QByteArray(
        "# Test Roadmap\n\n"
        "Intro paragraph that exists purely to pad the file past the\n"
        "1 KiB minimum-parseable-size gate the flip path enforces before\n"
        "it will trust an ants-v1 walk. Lorem ipsum dolor sit amet,\n"
        "consectetur adipiscing elit, sed do eiusmod tempor incididunt\n"
        "ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis\n"
        "nostrud exercitation ullamco laboris nisi ut aliquip ex ea\n"
        "commodo consequat. Duis aute irure dolor in reprehenderit in\n"
        "voluptate velit esse cillum dolore eu fugiat nulla pariatur.\n"
        "Excepteur sint occaecat cupidatat non proident, sunt in culpa\n"
        "qui officia deserunt mollit anim id est laborum. More padding\n"
        "to be safe and clear the gate with comfortable headroom for the\n"
        "parser and the size check above. Sed ut perspiciatis unde omnis\n"
        "iste natus error sit voluptatem accusantium doloremque laudantium,\n"
        "totam rem aperiam, eaque ipsa quae ab illo inventore veritatis et\n"
        "quasi architecto beatae vitae dicta sunt explicabo. Nemo enim\n"
        "ipsam voluptatem quia voluptas sit aspernatur aut odit aut fugit.\n"
        "\n"
        "## Work Items\n\n"
        "- \xF0\x9F\x93\x8B [ANTS-0042] **First bullet.**\n"
        "  Kind: chore.\n"
        "  Layman: The first seeded item, in one plain sentence.\n"
        "  Source: seed.\n"
        "- \xF0\x9F\x93\x8B [ANTS-0043] **Second bullet.**\n"
        "  Kind: chore.\n"
        "  Layman: The second seeded item, in one plain sentence.\n"
        "  Source: seed.\n"
        "- \xF0\x9F\x93\x8B [ANTS-0044] **Third bullet.**\n"
        "  Kind: chore.\n"
        "  Layman: The third seeded item, in one plain sentence.\n"
        "  Source: seed.\n"
        "\n");
}

bool writeFile(const QString &path, const QByteArray &body) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

QByteArray readFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

QString roadmapPath(const QString &root) {
    return QDir(root).filePath(QStringLiteral("ROADMAP.md"));
}

QJsonObject loc(const QString &id, const QString &note = QString()) {
    QJsonObject o;
    o[QStringLiteral("id")] = id;
    if (!note.isEmpty()) o[QStringLiteral("note")] = note;
    return o;
}

QJsonObject batchReq(const QString &root, const QString &op,
                     const QJsonArray &locators) {
    QJsonObject o;
    o[QStringLiteral("caller_cwd")] = root;
    o[QStringLiteral("op")]         = op;
    o[QStringLiteral("locators")]   = locators;
    return o;
}

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

// NEVER default-construct RoadmapStore: defaultPath() resolves the developer's
// REAL store under XDG_DATA_HOME. Every store case here redirects that first.
std::unique_ptr<RoadmapStore> openStore(RoadmapStore::Access access) {
    auto store = std::make_unique<RoadmapStore>(
        RoadmapStore::defaultPath(), RoadmapStore::kDefaultHistoryCapBytes,
        access);
    QString err;
    if (!store->open(&err)) {
        ADD_FAILURE() << "store open: " << err.toStdString();
        return nullptr;
    }
    return store;
}

// Writes the fixture and returns the CANONICAL root (the store keys a project
// on it, ANTS-3756 INV-8, and /tmp is a symlink on some hosts).
QString seedProject(ants_test::XdgGuard &guard, const QTemporaryDir &tmp) {
    guard.setEnv("XDG_DATA_HOME",
                 QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
    const QString raw = QDir(tmp.path()).filePath(QStringLiteral("proj"));
    if (!writeFile(roadmapPath(raw), seedV1())) return QString();
    return QFileInfo(raw).canonicalFilePath();
}

bool migrate(const QString &root) {
    auto store = openStore(RoadmapStore::Access::Bulk);
    if (!store) return false;
    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    if (!disc) {
        ADD_FAILURE() << "findRoadmaps: " << err.toStdString();
        return false;
    }
    const auto plan = RoadmapMigrate::planFrom(*disc, QStringLiteral("Demo"),
                                               QStringLiteral("ants"));
    RoadmapMigrateLoad::Options opts;
    opts.changedAt   = QStringLiteral("2026-08-24T10:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(*store, plan, opts);
    if (!out.ok) {
        ADD_FAILURE() << "migration load: " << out.error.toStdString();
        return false;
    }
    return true;
}

}  // namespace

// ── INV-1 / INV-2 — the batch appends N notes and flips nothing ────────────

TEST(roadmap_log_annotate_batch, Inv1AppendsEveryNoteAndFlipsNothing) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1()));

    RemoteControl rc(nullptr);
    QJsonArray locs;
    locs.append(loc(QStringLiteral("ANTS-0042"), QStringLiteral("Note one.")));
    locs.append(loc(QStringLiteral("ANTS-0043"), QStringLiteral("Note two.")));
    locs.append(loc(QStringLiteral("ANTS-0044"), QStringLiteral("Note three.")));
    const QJsonObject resp = rc.cmdRoadmapLogFlipBatchForTest(
        batchReq(tmp.path(), QStringLiteral("annotate_batch"), locs)).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("op")).toString(),
              QStringLiteral("annotate_batch"));
    EXPECT_EQ(resp.value(QStringLiteral("flipped_count")).toInt(), 3);
    EXPECT_EQ(resp.value(QStringLiteral("skipped_count")).toInt(), 0);

    const std::string md = readFile(roadmapPath(tmp.path())).toStdString();
    EXPECT_TRUE(contains(md, "Note one."));
    EXPECT_TRUE(contains(md, "Note two."));
    EXPECT_TRUE(contains(md, "Note three."));
    // INV-1 — every status emoji is exactly as seeded. This is the assertion
    // that separates annotate_batch from flip_batch-with-notes.
    EXPECT_TRUE(contains(md, "- \xF0\x9F\x93\x8B [ANTS-0042] **First bullet.**"));
    EXPECT_TRUE(contains(md, "- \xF0\x9F\x93\x8B [ANTS-0043] **Second bullet.**"));
    EXPECT_TRUE(contains(md, "- \xF0\x9F\x93\x8B [ANTS-0044] **Third bullet.**"));
    EXPECT_FALSE(contains(md, "\xE2\x9C\x85 [ANTS-00"))
        << "annotate_batch must not ship a status emoji anywhere";
}

// INV-5 — no batch-wide to_status; each row carries its own unchanged status.
TEST(roadmap_log_annotate_batch, Inv5NoBatchWideToStatus) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1()));

    RemoteControl rc(nullptr);
    QJsonArray locs;
    locs.append(loc(QStringLiteral("ANTS-0042"), QStringLiteral("Note one.")));
    const QJsonObject resp = rc.cmdRoadmapLogFlipBatchForTest(
        batchReq(tmp.path(), QStringLiteral("annotate_batch"), locs)).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(resp.contains(QStringLiteral("to_status")))
        << "a single batch-wide to_status could only be wrong under annotate";

    const QJsonArray rows = resp.value(QStringLiteral("flipped")).toArray();
    ASSERT_EQ(rows.size(), 1);
    const QJsonObject row = rows.at(0).toObject();
    EXPECT_EQ(row.value(QStringLiteral("to_status")).toString(),
              QString::fromUtf8("\xF0\x9F\x93\x8B"));
    EXPECT_EQ(row.value(QStringLiteral("from_status")).toString(),
              row.value(QStringLiteral("to_status")).toString());
}

// INV-3 — to_status is refused, not ignored, and nothing is written.
TEST(roadmap_log_annotate_batch, Inv3RefusesToStatus) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1()));
    const QByteArray before = readFile(roadmapPath(tmp.path()));

    RemoteControl rc(nullptr);
    QJsonArray locs;
    locs.append(loc(QStringLiteral("ANTS-0042"), QStringLiteral("Note one.")));
    QJsonObject req =
        batchReq(tmp.path(), QStringLiteral("annotate_batch"), locs);
    req[QStringLiteral("to_status")] = QStringLiteral("shipped");
    const QJsonObject resp =
        rc.cmdRoadmapLogFlipBatchForTest(req).object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_op_combo"));
    EXPECT_EQ(readFile(roadmapPath(tmp.path())), before)
        << "a refused annotate_batch must not touch the file";
}

// INV-4 — a noteless locator is skipped; its siblings still apply.
TEST(roadmap_log_annotate_batch, Inv4NotelessLocatorSkippedNotBatch) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1()));

    RemoteControl rc(nullptr);
    QJsonArray locs;
    locs.append(loc(QStringLiteral("ANTS-0042"), QStringLiteral("Kept note.")));
    locs.append(loc(QStringLiteral("ANTS-0043")));            // no note
    const QJsonObject resp = rc.cmdRoadmapLogFlipBatchForTest(
        batchReq(tmp.path(), QStringLiteral("annotate_batch"), locs)).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("flipped_count")).toInt(), 1);
    ASSERT_EQ(resp.value(QStringLiteral("skipped_count")).toInt(), 1);
    const QJsonObject s =
        resp.value(QStringLiteral("skipped")).toArray().at(0).toObject();
    EXPECT_EQ(s.value(QStringLiteral("code")).toString(),
              QStringLiteral("missing_field"));
    EXPECT_EQ(s.value(QStringLiteral("locator_index")).toInt(), 1);

    EXPECT_TRUE(contains(readFile(roadmapPath(tmp.path())).toStdString(),
                         "Kept note."));
}

// INV-4 — every locator noteless still refuses the whole call.
TEST(roadmap_log_annotate_batch, Inv4AllNotelessRefusesWholeCall) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1()));
    const QByteArray before = readFile(roadmapPath(tmp.path()));

    RemoteControl rc(nullptr);
    QJsonArray locs;
    locs.append(loc(QStringLiteral("ANTS-0042")));
    locs.append(loc(QStringLiteral("ANTS-0043")));
    const QJsonObject resp = rc.cmdRoadmapLogFlipBatchForTest(
        batchReq(tmp.path(), QStringLiteral("annotate_batch"), locs)).object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("missing_field"));
    EXPECT_EQ(resp.value(QStringLiteral("op")).toString(),
              QStringLiteral("annotate_batch"));
    EXPECT_EQ(readFile(roadmapPath(tmp.path())), before);
}

// INV-7 — the markdown path names itself.
TEST(roadmap_log_annotate_batch, Inv7MarkdownPathDeclaresPatch) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1()));

    RemoteControl rc(nullptr);
    QJsonArray locs;
    locs.append(loc(QStringLiteral("ANTS-0042"), QStringLiteral("Note one.")));
    const QJsonObject resp = rc.cmdRoadmapLogFlipBatchForTest(
        batchReq(tmp.path(), QStringLiteral("annotate_batch"), locs)).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("write_path")).toString(),
              QStringLiteral("patch"));
}

// INV-7 — and so does the single-item op, on the same path.
TEST(roadmap_log_annotate_batch, Inv7SingleAnnotateDeclaresPatch) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1()));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("annotate");
    req[QStringLiteral("id")]         = QStringLiteral("ANTS-0042");
    req[QStringLiteral("note")]       = QStringLiteral("Single note.");
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(req).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("write_path")).toString(),
              QStringLiteral("patch"));
    // The patch path still carries the fields ANTS-4464 found missing on the
    // other one — this is the shape the reporter saw on their FIRST call.
    EXPECT_TRUE(resp.contains(QStringLiteral("note_line")));
    EXPECT_TRUE(resp.contains(QStringLiteral("line")));
}

// ── INV-6 — the reported divergence, on the store path ─────────────────────

// The store says shipped, the file says planned, and the envelope must report
// what the render committed. Before ANTS-4466 this returned 📋.
TEST(roadmap_log_annotate_batch, Inv6StorePathReportsStoreStatusNotFile) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedProject(guard, tmp);
    ASSERT_FALSE(root.isEmpty());
    ASSERT_TRUE(migrate(root));

    RemoteControl rc(nullptr);

    // 1. Flip in the store, through the store path.
    QJsonObject flip;
    flip[QStringLiteral("caller_cwd")] = root;
    flip[QStringLiteral("op")]         = QStringLiteral("flip");
    flip[QStringLiteral("id")]         = QStringLiteral("ANTS-0042");
    flip[QStringLiteral("to_status")]  = QStringLiteral("shipped");
    const QJsonObject flipResp = rc.cmdRoadmapLogFlipForTest(flip).object();
    ASSERT_TRUE(flipResp.value(QStringLiteral("ok")).toBool())
        << flipResp.value(QStringLiteral("error")).toString().toStdString();
    ASSERT_EQ(flipResp.value(QStringLiteral("write_path")).toString(),
              QStringLiteral("render"))
        << "this case is only meaningful on the store path";

    // 2. Revert the FILE underneath the store — the reported `git checkout --`.
    ASSERT_TRUE(writeFile(roadmapPath(root), seedV1()));

    // 3. Annotate. The render restores ✅; the envelope must say so.
    QJsonObject ann;
    ann[QStringLiteral("caller_cwd")] = root;
    ann[QStringLiteral("op")]         = QStringLiteral("annotate");
    ann[QStringLiteral("id")]         = QStringLiteral("ANTS-0042");
    ann[QStringLiteral("note")]       = QStringLiteral("Progress note.");
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(ann).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("to_status")).toString(),
              QString::fromUtf8("\xE2\x9C\x85"))
        << "to_status must be the committed result, not the stale file read";
    EXPECT_EQ(resp.value(QStringLiteral("from_status")).toString(),
              QString::fromUtf8("\xE2\x9C\x85"));
    // INV-6 — the divergence is surfaced rather than resolved silently.
    EXPECT_EQ(resp.value(QStringLiteral("file_status")).toString(),
              QString::fromUtf8("\xF0\x9F\x93\x8B"));
    EXPECT_EQ(resp.value(QStringLiteral("write_path")).toString(),
              QStringLiteral("render"));
}

// INV-6 — file_status rides the true arm: absent when the two agree.
TEST(roadmap_log_annotate_batch, Inv6NoFileStatusWhenStoreAndFileAgree) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedProject(guard, tmp);
    ASSERT_FALSE(root.isEmpty());
    ASSERT_TRUE(migrate(root));

    RemoteControl rc(nullptr);
    QJsonObject ann;
    ann[QStringLiteral("caller_cwd")] = root;
    ann[QStringLiteral("op")]         = QStringLiteral("annotate");
    ann[QStringLiteral("id")]         = QStringLiteral("ANTS-0042");
    ann[QStringLiteral("note")]       = QStringLiteral("Healthy note.");
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(ann).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    ASSERT_EQ(resp.value(QStringLiteral("write_path")).toString(),
              QStringLiteral("render"));
    EXPECT_FALSE(resp.contains(QStringLiteral("file_status")))
        << "a key present on every write restating from_status is unread";
}

// INV-8 — the store path honours return:"headline_only".
TEST(roadmap_log_annotate_batch, Inv8StorePathEmitsPostBullets) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedProject(guard, tmp);
    ASSERT_FALSE(root.isEmpty());
    ASSERT_TRUE(migrate(root));

    RemoteControl rc(nullptr);
    QJsonObject ann;
    ann[QStringLiteral("caller_cwd")] = root;
    ann[QStringLiteral("op")]         = QStringLiteral("annotate");
    ann[QStringLiteral("id")]         = QStringLiteral("ANTS-0042");
    ann[QStringLiteral("note")]       = QStringLiteral("Echoed note.");
    ann[QStringLiteral("return")]     = QStringLiteral("headline_only");
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(ann).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    ASSERT_EQ(resp.value(QStringLiteral("write_path")).toString(),
              QStringLiteral("render"));
    const QJsonArray pb = resp.value(QStringLiteral("post_bullets")).toArray();
    ASSERT_EQ(pb.size(), 1);
    EXPECT_EQ(pb.at(0).toObject().value(QStringLiteral("id")).toString(),
              QStringLiteral("ANTS-0042"));
    EXPECT_EQ(pb.at(0).toObject().value(QStringLiteral("status")).toString(),
              QStringLiteral("planned"));
}

// INV-1 on the store path — annotate_batch writes N notes and no status.
TEST(roadmap_log_annotate_batch, Inv1StorePathBatchLeavesStatusAlone) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedProject(guard, tmp);
    ASSERT_FALSE(root.isEmpty());
    ASSERT_TRUE(migrate(root));

    RemoteControl rc(nullptr);
    QJsonArray locs;
    locs.append(loc(QStringLiteral("ANTS-0042"), QStringLiteral("Store note A.")));
    locs.append(loc(QStringLiteral("ANTS-0043"), QStringLiteral("Store note B.")));
    const QJsonObject resp = rc.cmdRoadmapLogFlipBatchForTest(
        batchReq(root, QStringLiteral("annotate_batch"), locs)).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("op")).toString(),
              QStringLiteral("annotate_batch"));
    EXPECT_EQ(resp.value(QStringLiteral("write_path")).toString(),
              QStringLiteral("render"));
    EXPECT_FALSE(resp.contains(QStringLiteral("to_status")));

    const std::string md = readFile(roadmapPath(root)).toStdString();
    EXPECT_TRUE(contains(md, "Store note A."));
    EXPECT_TRUE(contains(md, "Store note B."));
    EXPECT_FALSE(contains(md, "\xE2\x9C\x85 [ANTS-00"))
        << "annotate_batch must not flip anything on the store path either";
}
