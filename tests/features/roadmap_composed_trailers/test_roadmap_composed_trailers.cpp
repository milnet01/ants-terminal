// ANTS-5087 — `composed_trailers` must name exactly the trailer lines the
// render wrote. Contract: tests/features/roadmap_composed_trailers/spec.md
//
// Behavioural, through roadmap_query (the read seam) and roadmap_log
// op:"render" (the publish). The read seam computed its own copy of the
// render's five emission predicates, and two of them had drifted: `source`
// lacked the defaulted-provenance rider (ANTS-4065 § 2.4) and `kind` lacked the
// unrecognised-value rider. So the field could name a `Source:` line the render
// withheld, and miss a `Kind:` line the render wrote.
//
// Every case asserts the field against the PUBLISHED FILE rather than against
// the predicate. A test that re-states the predicate passes by agreeing with
// itself, which is how the two copies drifted apart unnoticed.

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
#include <QStringLiteral>
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

// NEVER default-construct RoadmapStore: defaultPath() resolves the developer's
// REAL machine-global store under XDG_DATA_HOME.
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
    "magni dolores eos qui ratione voluptatem sequi nesciunt neque porro.\n"
    "Quisquam est, qui dolorem ipsum quia dolor sit amet, consectetur, adipisci\n"
    "velit, sed quia non numquam eius modi tempora incidunt ut labore.\n";

// Three shipped items, so the render's Layman gate judges none of them and each
// case is about the trailer lines alone.
QByteArray fixture() {
    QByteArray b =
        "<!-- ants-roadmap-format: 1 -->\n"
        "\n"
        "# Demo \xE2\x80\x94 Roadmap\n"
        "\n";
    b += kPad;
    b += "\n"
        "## Work\n"
        "\n"
        // No Source: line at all, so the import supplies one and marks the
        // provenance `defaulted` — the state whose Source: line the render
        // withholds (ANTS-4065 § 2.4). Ends on its trailer run, so ANTS-4506
        // strips those lines out of the stored body and the values live in the
        // columns alone.
        "- \xE2\x9C\x85 [CTR-0001] **An item that never declared a source.**\n"
        "  Some prose about it.\n"
        "  Layman: A thing whose source nobody wrote down.\n"
        "  Kind: fix.\n"
        "\n"
        // Ends on PROSE, so every declaration stays in the stored body and
        // shadows its column. The Kind: value is NOT in the vocabulary, which
        // is the one case where a shadowing declaration does not suppress the
        // render's own line.
        "- \xE2\x9C\x85 [CTR-0002] **An item whose body declares an unknown kind.**\n"
        "  Layman: A thing with a kind nobody recognises.\n"
        "  Kind: notarealkind.\n"
        "  Source: seed.\n"
        "  Closing prose line.\n"
        "\n"
        // The control: declares everything, ends on prose, and every
        // declaration is recognised — so the render writes no trailer line of
        // its own and nothing is composed.
        "- \xE2\x9C\x85 [CTR-0003] **An item that declares everything plainly.**\n"
        "  Layman: A thing with every trailer in its body.\n"
        "  Kind: fix.\n"
        "  Source: seed.\n"
        "  Closing prose line.\n"
        "\n";
    return b;
}

QString seedMigrated(ants_test::XdgGuard &guard, const QTemporaryDir &tmp,
                     qint64 *projectId) {
    guard.setEnv("XDG_DATA_HOME",
                 QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
    const QString rawRoot = QDir(tmp.path()).filePath(QStringLiteral("proj"));
    if (!writeFile(QDir(rawRoot).filePath(QStringLiteral("ROADMAP.md")), fixture()))
        return QString();
    QString root = QFileInfo(rawRoot).canonicalFilePath();

    auto store = openStore(RoadmapStore::Access::Bulk);
    if (!store) return QString();
    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    if (!disc) { ADD_FAILURE() << "findRoadmaps: " << err.toStdString(); return QString(); }
    const auto plan =
        RoadmapMigrate::planFrom(*disc, QStringLiteral("Demo"), QStringLiteral("ctr"));
    RoadmapMigrateLoad::Options opts;
    opts.changedAt   = QStringLiteral("2026-09-18T10:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(*store, plan, opts);
    if (!out.ok) { ADD_FAILURE() << "migration load: " << out.error.toStdString(); return QString(); }
    *projectId = out.projectId;
    return root;
}

struct Fx {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    qint64 projectId = 0;
    QString root;
    bool ok() {
        if (!tmp.isValid()) return false;
        root = seedMigrated(guard, tmp, &projectId);
        return !root.isEmpty();
    }
    QString roadmap() const {
        return QDir(root).filePath(QStringLiteral("ROADMAP.md"));
    }
};

// The trailer keys the read seam says the render composed, for one id.
QStringList composedFor(RemoteControl &rc, const QString &root, const QString &id) {
    QJsonObject req;
    req[QStringLiteral("caller_cwd")]   = root;
    req[QStringLiteral("id")]           = id;
    req[QStringLiteral("include_body")] = true;
    const QJsonObject resp = rc.cmdRoadmapQuery(req).object();
    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    const QJsonObject bullet =
        resp.value(QStringLiteral("bullets")).toArray().at(0).toObject();
    QStringList out;
    for (const auto &v :
             bullet.value(QStringLiteral("composed_trailers")).toArray())
        out << v.toString();
    out.sort();
    return out;
}

// One bullet's block of the published file: its head line through the line
// before the next bullet (or EOF). Anchored on the id, so it moves with the
// file rather than on a line number.
QByteArray blockFor(const QByteArray &published, const char *id) {
    const int head = published.indexOf(QByteArray("[") + id + "]");
    if (head < 0) return {};
    const int start = published.lastIndexOf('\n', head) + 1;
    const int next  = published.indexOf("\n- ", head);
    return next < 0 ? published.mid(start)
                    : published.mid(start, next - start);
}

int countLines(const QByteArray &block, const char *needle) {
    int n = 0;
    for (const QByteArray &line : block.split('\n')) {
        if (line.trimmed().startsWith(needle)) ++n;
    }
    return n;
}

// Publish the store over the file, so every assertion below is against text the
// render actually wrote.
void publish(RemoteControl &rc, const QString &root) {
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("op")]         = QStringLiteral("render");
    const QJsonObject resp = rc.cmdRoadmapLogRenderForTest(req).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
}

}  // namespace

// A `source` the import supplied is not written back into the file (ANTS-4065
// § 2.4), so it was never composed — and the read seam said it was.
TEST(RoadmapComposedTrailers, Ants5087DefaultedSourceIsNotComposed) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    publish(rc, fx.root);

    const QByteArray block = blockFor(readAll(fx.roadmap()), "CTR-0001");
    ASSERT_FALSE(block.isEmpty()) << "CTR-0001 is not in the published file";
    ASSERT_EQ(countLines(block, "Source:"), 0)
        << "precondition: the render withholds a defaulted Source:\n"
        << block.toStdString();

    EXPECT_FALSE(composedFor(rc, fx.root, QStringLiteral("CTR-0001"))
                     .contains(QStringLiteral("source")))
        << "no Source: line was written, so none was composed";
}

// The same item's other two keys ARE composed: their values live in the columns
// alone, so the render wrote both lines. Without this the case above would pass
// on an empty list.
TEST(RoadmapComposedTrailers, Ants5087ColumnOnlyKeysAreStillComposed) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    publish(rc, fx.root);

    const QByteArray block = blockFor(readAll(fx.roadmap()), "CTR-0001");
    ASSERT_FALSE(block.isEmpty());
    ASSERT_EQ(countLines(block, "**Layman:**"), 1) << block.toStdString();
    ASSERT_EQ(countLines(block, "Kind:"), 1) << block.toStdString();

    const QStringList composed =
        composedFor(rc, fx.root, QStringLiteral("CTR-0001"));
    EXPECT_TRUE(composed.contains(QStringLiteral("layman"))) << composed.join(',').toStdString();
    EXPECT_TRUE(composed.contains(QStringLiteral("kind"))) << composed.join(',').toStdString();
}

// An unrecognised line-initial `Kind:` does not suppress the render's own —
// otherwise the recognised column would be dropped and the next parse would
// adopt the fragment. The render writes both lines; the read seam saw a
// shadowing declaration and reported nothing composed.
TEST(RoadmapComposedTrailers, Ants5087UnrecognisedBodyKindIsStillComposed) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    publish(rc, fx.root);

    const QByteArray block = blockFor(readAll(fx.roadmap()), "CTR-0002");
    ASSERT_FALSE(block.isEmpty()) << "CTR-0002 is not in the published file";
    ASSERT_EQ(countLines(block, "Kind:"), 2)
        << "precondition: the body's unrecognised Kind: line AND the render's "
           "own, both present\n"
        << block.toStdString();

    EXPECT_TRUE(composedFor(rc, fx.root, QStringLiteral("CTR-0002"))
                    .contains(QStringLiteral("kind")))
        << "the render wrote a Kind: line, so `kind` was composed";
}

// The control: every declaration recognised and in the body, so the render
// writes no trailer line of its own and nothing is composed. This is what keeps
// the two cases above from passing under a predicate that simply says yes.
TEST(RoadmapComposedTrailers, Ants5087FullyDeclaredBodyComposesNothing) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    publish(rc, fx.root);

    const QByteArray block = blockFor(readAll(fx.roadmap()), "CTR-0003");
    ASSERT_FALSE(block.isEmpty());
    ASSERT_EQ(countLines(block, "Kind:"), 1)
        << "precondition: the body's own line and no second\n"
        << block.toStdString();

    EXPECT_TRUE(composedFor(rc, fx.root, QStringLiteral("CTR-0003")).isEmpty())
        << "a body that declares every key leaves the render nothing to write";
}
