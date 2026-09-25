// Feature-conformance test for ANTS-4485 — on a store-backed project the
// locating roadmap_log ops find their target in the store. Contract:
// tests/features/roadmap_store_locate/spec.md, implementing
// docs/specs/ANTS-4485-store-backed-locate.md § 5.
//
// "In the store, absent from the file" is produced the way it happens in use:
// migrate, then take the bullet out of ROADMAP.md by hand.

#include "../../_support/expect.h"
#include "../../_support/xdg_guard.h"

#include "remotecontrol.h"
#include "roadmapmigrate.h"
#include "roadmapmigrateload.h"
#include "roadmapstore.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <memory>

ANTS_TEST_SCOPE();

namespace {

bool writeFile(const QString &path, const QByteArray &body) {
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

QByteArray readAll(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    return f.readAll();
}

// NEVER default-construct RoadmapStore without redirecting XDG_DATA_HOME first:
// defaultPath() is the developer's real, machine-global store.
std::unique_ptr<RoadmapStore> openStore(RoadmapStore::Access access) {
    auto store = std::make_unique<RoadmapStore>(
        RoadmapStore::defaultPath(), RoadmapStore::kDefaultHistoryCapBytes, access);
    QString err;
    if (!store->open(&err)) {
        ADD_FAILURE() << "store open: " << err.toStdString();
        return nullptr;
    }
    return store;
}

// Past kRoadmapMinParseableSize, below which the write paths refuse to trust
// an ants-v1 walk.
const char *kPad =
    "Intro paragraph that exists purely to pad this fixture past the 1 KiB\n"
    "minimum-parseable-size gate the roadmap_log write paths enforce before\n"
    "they will trust an ants-v1 walk. Lorem ipsum dolor sit amet, consectetur\n"
    "adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore\n"
    "magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco\n"
    "laboris nisi ut aliquip ex ea commodo consequat. Duis aute irure dolor in\n"
    "reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla\n"
    "pariatur. Excepteur sint occaecat cupidatat non proident, sunt in culpa\n"
    "qui officia deserunt mollit anim id est laborum. Sed ut perspiciatis unde\n"
    "omnis iste natus error sit voluptatem accusantium doloremque laudantium,\n"
    "totam rem aperiam, eaque ipsa quae ab illo inventore veritatis et quasi\n"
    "architecto beatae vitae dicta sunt explicabo. Nemo enim ipsam voluptatem\n"
    "quia voluptas sit aspernatur aut odit aut fugit, sed quia consequuntur\n"
    "magni dolores eos qui ratione voluptatem sequi nesciunt neque porro.\n";

QByteArray bullet(const char *emoji, const char *id, const char *headline) {
    return QByteArray("- ") + emoji + " [" + id + "] **" + headline + "**\n"
           "  Layman: A thing.\n"
           "  Kind: implement.\n"
           "  Source: seed.\n"
           "\n";
}

const char *kPlanned = "\xF0\x9F\x93\x8B";
const char *kShipped = "\xE2\x9C\x85";

// DEMO-0010 and DEMO-0011 share a headline, for INV-5.
QByteArray fixture() {
    QByteArray b =
        "<!-- ants-roadmap-format: 1 -->\n"
        "\n"
        "# Demo \xE2\x80\x94 Roadmap\n"
        "\n";
    b += kPad;
    b += "\n## Work\n\n";
    b += bullet(kPlanned, "DEMO-0007", "An open item.");
    b += bullet(kShipped, "DEMO-0003", "A shipped item.");
    b += bullet(kPlanned, "DEMO-0010", "Twin.");
    b += bullet(kPlanned, "DEMO-0011", "Twin.");
    return b;
}

QString canonicalRoot(const QTemporaryDir &tmp) {
    return QFileInfo(QDir(tmp.path()).filePath(QStringLiteral("proj")))
        .canonicalFilePath();
}

// Writes the fixture and, when `migrate`, loads it into a store under the
// case's own XDG_DATA_HOME.
QString seed(ants_test::XdgGuard &guard, const QTemporaryDir &tmp, bool migrate,
             qint64 *projectId = nullptr) {
    guard.setEnv("XDG_DATA_HOME",
                 QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
    const QString rawRoot = QDir(tmp.path()).filePath(QStringLiteral("proj"));
    if (!writeFile(rawRoot + QStringLiteral("/ROADMAP.md"), fixture()))
        return QString();
    const QString root = canonicalRoot(tmp);
    if (!migrate) return root;

    auto store = openStore(RoadmapStore::Access::Bulk);
    if (!store) return QString();
    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    if (!disc) {
        ADD_FAILURE() << "findRoadmaps: " << err.toStdString();
        return QString();
    }
    const auto plan =
        RoadmapMigrate::planFrom(*disc, QStringLiteral("Demo"), QStringLiteral("demo"));
    RoadmapMigrateLoad::Options opts;
    opts.changedAt   = QStringLiteral("2026-09-25T10:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(*store, plan, opts);
    if (!out.ok) {
        ADD_FAILURE() << "migration load: " << out.error.toStdString();
        return QString();
    }
    if (projectId) *projectId = out.projectId;
    return root;
}

QString roadmapOf(const QString &root) {
    return root + QStringLiteral("/ROADMAP.md");
}

// Take every bullet carrying one of `ids` out of ROADMAP.md, continuation
// lines included, leaving the store untouched.
bool dropFromFile(const QString &root, const QStringList &ids) {
    const QStringList lines =
        QString::fromUtf8(readAll(roadmapOf(root))).split(QLatin1Char('\n'));
    QStringList kept;
    bool skipping = false;
    for (const QString &ln : lines) {
        if (ln.startsWith(QStringLiteral("- "))) {
            skipping = false;
            for (const QString &id : ids)
                if (ln.contains(QLatin1Char('[') + id + QLatin1Char(']')))
                    skipping = true;
        } else if (!ln.startsWith(QStringLiteral("  "))) {
            skipping = false;
        }
        if (!skipping) kept << ln;
    }
    return writeFile(roadmapOf(root), kept.join(QLatin1Char('\n')).toUtf8());
}

QString statusOf(qint64 projectId, const QString &id) {
    auto store = openStore(RoadmapStore::Access::Interactive);
    if (!store) return QString();
    QString err;
    const auto pk = store->findItem(projectId, id, &err);
    if (!pk) return QString();
    const auto item = store->readItem(*pk, &err);
    return item ? item->status : QString();
}

QJsonObject base(const QString &root, const char *op) {
    QJsonObject r;
    r[QStringLiteral("caller_cwd")] = root;
    r[QStringLiteral("op")]         = QString::fromLatin1(op);
    return r;
}

QJsonObject flipReq(const QString &root, const QString &id) {
    QJsonObject r = base(root, "flip");
    r[QStringLiteral("id")]        = id;
    r[QStringLiteral("to_status")] = QStringLiteral("in-progress");
    return r;
}

QJsonObject flipBatchReq(const QString &root, const QJsonObject &loc) {
    QJsonObject r = base(root, "flip_batch");
    r[QStringLiteral("to_status")] = QStringLiteral("in-progress");
    r[QStringLiteral("locators")]  = QJsonArray{loc};
    return r;
}

QJsonObject setBodyReq(const QString &root, const QString &id) {
    QJsonObject r = base(root, "set_body");
    r[QStringLiteral("id")]       = id;
    r[QStringLiteral("new_text")] = QStringLiteral("A replaced body.");
    return r;
}

QJsonObject amendFieldReq(const QString &root, const QString &id) {
    QJsonObject r = base(root, "amend_field");
    r[QStringLiteral("id")]    = id;
    r[QStringLiteral("field")] = QStringLiteral("layman");
    r[QStringLiteral("value")] = QStringLiteral("A replaced summary.");
    return r;
}

std::string dump(const QJsonObject &o) {
    return QJsonDocument(o).toJson().toStdString();
}

}  // namespace

// INV-1 — an id roadmap_query returns is writable by one op per handler, with
// the item absent from ROADMAP.md.
TEST(roadmap_store_locate, queryableIsWritable) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 pid = 0;
    const QString root = seed(guard, tmp, true, &pid);
    ASSERT_FALSE(root.isEmpty());
    const QString id = QStringLiteral("DEMO-0007");
    RemoteControl rc(nullptr);

    // Each write re-renders the file with the item back in it, so it is taken
    // out again before every op.
    ASSERT_TRUE(dropFromFile(root, {id}));
    const QJsonObject flip = rc.cmdRoadmapLogFlipForTest(flipReq(root, id)).object();
    EXPECT_TRUE(flip.value(QStringLiteral("ok")).toBool()) << "flip: " << dump(flip);

    ASSERT_TRUE(dropFromFile(root, {id}));
    QJsonObject loc;
    loc[QStringLiteral("id")] = id;
    QJsonObject batchReq = flipBatchReq(root, loc);
    batchReq[QStringLiteral("to_status")] = QStringLiteral("shipped");
    const QJsonObject batch = rc.cmdRoadmapLogFlipBatchForTest(batchReq).object();
    EXPECT_TRUE(batch.value(QStringLiteral("ok")).toBool()) << "flip_batch: " << dump(batch);
    EXPECT_EQ(batch.value(QStringLiteral("flipped_count")).toInt(), 1) << dump(batch);

    ASSERT_TRUE(dropFromFile(root, {id}));
    const QJsonObject body = rc.cmdRoadmapLogSetBodyForTest(setBodyReq(root, id)).object();
    EXPECT_TRUE(body.value(QStringLiteral("ok")).toBool()) << "set_body: " << dump(body);

    ASSERT_TRUE(dropFromFile(root, {id}));
    const QJsonObject field =
        rc.cmdRoadmapLogAmendFieldForTest(amendFieldReq(root, id)).object();
    EXPECT_TRUE(field.value(QStringLiteral("ok")).toBool()) << "amend_field: " << dump(field);

    EXPECT_EQ(statusOf(pid, id), QStringLiteral("shipped"));
}

// INV-2 — in the file, absent from the store: the refusal names the cause and
// the remedy, and nothing is written.
TEST(roadmap_store_locate, fileOnlyRefusesWithCause) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 pid = 0;
    const QString root = seed(guard, tmp, true, &pid);
    ASSERT_FALSE(root.isEmpty());
    QByteArray text = readAll(roadmapOf(root));
    text += bullet(kPlanned, "DEMO-0099", "Added by hand.");
    ASSERT_TRUE(writeFile(roadmapOf(root), text));

    RemoteControl rc(nullptr);
    const QJsonObject resp =
        rc.cmdRoadmapLogFlipForTest(flipReq(root, QStringLiteral("DEMO-0099"))).object();
    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool()) << dump(resp);
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("bullet_not_found")) << dump(resp);
    const QString error = resp.value(QStringLiteral("error")).toString();
    EXPECT_TRUE(error.contains(QStringLiteral("not in this project's store")))
        << dump(resp);
    EXPECT_TRUE(error.contains(QStringLiteral("roadmap_migrate"))) << dump(resp);
    EXPECT_EQ(readAll(roadmapOf(root)), text);
}

// INV-3 — an id and a headline naming different items: the id wins.
TEST(roadmap_store_locate, locatorPrecedenceHolds) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 pid = 0;
    const QString root = seed(guard, tmp, true, &pid);
    ASSERT_FALSE(root.isEmpty());
    RemoteControl rc(nullptr);
    QJsonObject loc;
    loc[QStringLiteral("id")]       = QStringLiteral("DEMO-0007");
    loc[QStringLiteral("headline")] = QStringLiteral("A shipped item.");
    const QJsonObject resp = rc.cmdRoadmapLogFlipBatchForTest(flipBatchReq(root, loc)).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool()) << dump(resp);
    EXPECT_EQ(statusOf(pid, QStringLiteral("DEMO-0007")), QStringLiteral("in-progress"));
    EXPECT_EQ(statusOf(pid, QStringLiteral("DEMO-0003")), QStringLiteral("shipped"));
}

// INV-4 — a project the store does not serve answers exactly as before. The
// golden files were captured from the pre-change build; ANTS_STORE_LOCATE_RECORD
// rewrites them, and must never be set when checking a change.
TEST(roadmap_store_locate, markdownUnchanged) {
    const QString goldenDir =
        QStringLiteral(ANTS_SOURCE_DIR) + QStringLiteral("/tests/features/roadmap_store_locate/golden");
    const bool record = qEnvironmentVariableIsSet("ANTS_STORE_LOCATE_RECORD");

    struct Case { const char *name; QJsonObject (*make)(const QString &); int handler; };
    const Case cases[] = {
        {"flip", [](const QString &r) { return flipReq(r, QStringLiteral("DEMO-0007")); }, 0},
        {"flip_batch", [](const QString &r) {
             QJsonObject loc;
             loc[QStringLiteral("id")] = QStringLiteral("DEMO-0007");
             return flipBatchReq(r, loc); }, 1},
        {"amend_body", [](const QString &r) {
             QJsonObject q = base(r, "amend_body");
             q[QStringLiteral("id")]       = QStringLiteral("DEMO-0007");
             q[QStringLiteral("old_text")] = QStringLiteral("A thing.");
             q[QStringLiteral("new_text")] = QStringLiteral("Another thing.");
             return q; }, 2},
        {"amend_field", [](const QString &r) {
             return amendFieldReq(r, QStringLiteral("DEMO-0007")); }, 3},
    };
    for (const Case &c : cases) {
        ants_test::XdgGuard guard;
        QTemporaryDir tmp;
        ASSERT_TRUE(tmp.isValid());
        const QString root = seed(guard, tmp, false);
        ASSERT_FALSE(root.isEmpty());
        RemoteControl rc(nullptr);
        const QJsonObject req = c.make(root);
        QJsonObject resp;
        switch (c.handler) {
        case 0: resp = rc.cmdRoadmapLogFlipForTest(req).object(); break;
        case 1: resp = rc.cmdRoadmapLogFlipBatchForTest(req).object(); break;
        case 2: resp = rc.cmdRoadmapLogAmendBodyForTest(req).object(); break;
        default: resp = rc.cmdRoadmapLogAmendFieldForTest(req).object(); break;
        }
        // The sandbox path differs per run; nothing else may.
        QByteArray got = QJsonDocument(resp).toJson(QJsonDocument::Indented);
        got.replace(root.toUtf8(), "<ROOT>");
        got.replace(tmp.path().toUtf8(), "<TMP>");
        const QString path =
            goldenDir + QStringLiteral("/markdown-") + QString::fromLatin1(c.name) +
            QStringLiteral(".json");
        if (record) {
            ASSERT_TRUE(writeFile(path, got)) << path.toStdString();
            continue;
        }
        const QByteArray want = readAll(path);
        ASSERT_FALSE(want.isEmpty()) << "missing golden " << path.toStdString();
        EXPECT_EQ(got.toStdString(), want.toStdString()) << c.name;
    }
}

// INV-5 — a headline matching two stored items refuses bullet_ambiguous and
// changes neither. Both are out of the file, so only the store can answer.
TEST(roadmap_store_locate, ambiguousHeadlineRefuses) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 pid = 0;
    const QString root = seed(guard, tmp, true, &pid);
    ASSERT_FALSE(root.isEmpty());
    const QStringList twins{QStringLiteral("DEMO-0010"), QStringLiteral("DEMO-0011")};
    ASSERT_TRUE(dropFromFile(root, twins));
    RemoteControl rc(nullptr);
    QJsonObject req = base(root, "flip");
    req[QStringLiteral("headline")]  = QStringLiteral("Twin.");
    req[QStringLiteral("to_status")] = QStringLiteral("shipped");
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(req).object();
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("bullet_ambiguous")) << dump(resp);
    EXPECT_EQ(statusOf(pid, QStringLiteral("DEMO-0010")), QStringLiteral("planned"));
    EXPECT_EQ(statusOf(pid, QStringLiteral("DEMO-0011")), QStringLiteral("planned"));
}

// INV-6 — every locating handler refuses a line_range on a store-backed
// project with the same code.
TEST(roadmap_store_locate, lineRangeRefusedEverywhere) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 pid = 0;
    const QString root = seed(guard, tmp, true, &pid);
    ASSERT_FALSE(root.isEmpty());
    const QByteArray before = readAll(roadmapOf(root));
    const QJsonArray range{1, 400};
    RemoteControl rc(nullptr);

    QJsonObject flip = base(root, "flip");
    flip[QStringLiteral("line_range")] = range;
    flip[QStringLiteral("to_status")]  = QStringLiteral("shipped");
    const QJsonObject a = rc.cmdRoadmapLogFlipForTest(flip).object();
    EXPECT_EQ(a.value(QStringLiteral("code")).toString(),
              QStringLiteral("locator_unsupported")) << "flip: " << dump(a);

    QJsonObject loc;
    loc[QStringLiteral("line_range")] = range;
    const QJsonObject b = rc.cmdRoadmapLogFlipBatchForTest(flipBatchReq(root, loc)).object();
    const QJsonArray skipped = b.value(QStringLiteral("skipped")).toArray();
    ASSERT_EQ(skipped.size(), 1) << "flip_batch: " << dump(b);
    EXPECT_EQ(skipped.at(0).toObject().value(QStringLiteral("code")).toString(),
              QStringLiteral("locator_unsupported")) << dump(b);

    QJsonObject body = base(root, "amend_body");
    body[QStringLiteral("line_range")] = range;
    body[QStringLiteral("old_text")]   = QStringLiteral("A thing.");
    body[QStringLiteral("new_text")]   = QStringLiteral("x");
    const QJsonObject c = rc.cmdRoadmapLogAmendBodyForTest(body).object();
    EXPECT_EQ(c.value(QStringLiteral("code")).toString(),
              QStringLiteral("locator_unsupported")) << "amend_body: " << dump(c);

    QJsonObject field = amendFieldReq(root, QString());
    field.remove(QStringLiteral("id"));
    field[QStringLiteral("line_range")] = range;
    const QJsonObject d = rc.cmdRoadmapLogAmendFieldForTest(field).object();
    EXPECT_FALSE(d.value(QStringLiteral("ok")).toBool()) << "amend_field: " << dump(d);

    EXPECT_EQ(readAll(roadmapOf(root)), before);
}
