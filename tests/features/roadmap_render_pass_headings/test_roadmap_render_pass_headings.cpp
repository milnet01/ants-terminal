// ANTS-4803 — feature-conformance test for serving the `pass-headings`
// dialect from the store.
//
// The measured gap: the migration read this dialect correctly — sections,
// items, bodies, ids folded to PASS-N-M — but nothing rendered it back, so the
// store was WRITE-ONLY for such a project. Every store-side benefit (source of
// truth, op:"render", drift detection, cross-project reporting) was
// unavailable, and the reporting project de-registered twice rather than leave
// stale rows in the machine-global figures.
//
// The load-bearing case is INV-1: migrate, render, then migrate THAT and
// render again. Byte-stability across the second trip is what proves the
// render is the migration's inverse. Comparing against the author's own seed
// would be the wrong bar — a render canonicalises, exactly as the ants-v1 one
// does — and would fail for reasons that are not defects.

#include "../../_support/expect.h"

#include "passheadingwrite.h"
#include "roadmapmigrate.h"
#include "roadmapmigrateload.h"
#include "roadmaprender.h"
#include "roadmapsource.h"
#include "roadmapstore.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>

#include <memory>

ANTS_TEST_SCOPE();

namespace {

// A pass-headings roadmap: `#### Pass N.M` blocks, a `- **Status**:` line
// each, a sub-pass, and section headings around them.
const char *kSeed =
    "# Demo — Roadmap\n"
    "\n"
    "## Phase 4\n"
    "\n"
    "#### Pass 43.5 Harden the parser against a truncated frame.\n"
    "- **Status**: done\n"
    "  The frame length is validated before the body is read.\n"
    "\n"
    "#### Pass 43.5.B Follow-up: the same check on the reply path.\n"
    "- **Status**: todo\n"
    "\n"
    "## Phase 5\n"
    "\n"
    "#### Pass 44.1 Retire the legacy transport.\n"
    "- **Status**: in-progress\n"
    "  Blocked on the 43.5 follow-up.\n";

bool writeFile(const QString &path, const QByteArray &body) {
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

QString readAll(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    return QString::fromUtf8(f.readAll());
}

std::unique_ptr<RoadmapStore> openStore(const QString &path) {
    // Never a default-constructed RoadmapStore: that resolves defaultPath()
    // under XDG_DATA_HOME, i.e. the developer's REAL machine-global store.
    // Access::Bulk is named rather than defaulted — a migration load refuses
    // any other mode.
    auto store = std::make_unique<RoadmapStore>(
        path, RoadmapStore::kDefaultHistoryCapBytes, RoadmapStore::Access::Bulk);
    QString err;
    if (!store->open(&err)) {
        ADD_FAILURE() << "store open: " << err.toStdString();
        return nullptr;
    }
    return store;
}

// Migrate `md` at a fresh root, delete the seed, render the store back, and
// return what landed on disk. Deleting the seed is what stops a render that
// never ran from passing: without it the author's own bytes are still there
// and every assertion below would hold for the wrong reason.
QString migrateThenRender(QTemporaryDir &dir, const QString &leaf,
                          const QByteArray &md, bool *gateEngaged = nullptr) {
    const QString root = dir.filePath(leaf);
    if (!writeFile(root + QStringLiteral("/ROADMAP.md"), md)) {
        ADD_FAILURE() << "seed write failed"; return QString();
    }
    auto store = openStore(dir.filePath(leaf + QStringLiteral("-store.sqlite")));
    if (!store) return QString();

    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    if (!disc) { ADD_FAILURE() << "findRoadmaps: " << err.toStdString(); return QString(); }
    const auto plan = RoadmapMigrate::planFrom(
        *disc, QStringLiteral("Demo"), QStringLiteral("demo"));
    RoadmapMigrateLoad::Options lopts;
    lopts.changedAt   = QStringLiteral("2026-09-03T10:00:00Z");
    lopts.projectRoot = root;
    const auto loaded = RoadmapMigrateLoad::load(*store, plan, lopts);
    if (!loaded.ok) {
        ADD_FAILURE() << "migration load: " << loaded.error.toStdString();
        return QString();
    }

    if (gateEngaged) {
        // The gate this item was filed about: does the read seam now serve
        // this dialect from the store, or fall back to markdown?
        RoadmapSource::RoadmapText text = RoadmapSource::RoadmapText::fromFile(
            root + QStringLiteral("/ROADMAP.md"));
        *gateEngaged =
            RoadmapSource::migratedProject(*store, root, text).has_value();
    }

    if (!QFile::remove(root + QStringLiteral("/ROADMAP.md"))) {
        ADD_FAILURE() << "could not remove the seed before rendering";
        return QString();
    }
    RoadmapRender::Options ropts;
    ropts.liveRoadmapPath = QStringLiteral("ROADMAP.md");
    ropts.dialect         = QStringLiteral("pass-headings");
    const auto outcome =
        RoadmapRender::render(*store, loaded.projectId, root, ropts, &err);
    if (!outcome) { ADD_FAILURE() << "render: " << err.toStdString(); return QString(); }
    if (!outcome->gateFailures.isEmpty()) {
        ADD_FAILURE() << "render gate refused: "
                      << outcome->gateFailures.join(QStringLiteral(", ")).toStdString();
        return QString();
    }
    return readAll(root + QStringLiteral("/ROADMAP.md"));
}

}  // namespace

// INV-1 — migrate then render is byte-stable: rendering the render's own
// output reproduces it exactly. This is the property that makes the store
// safe to serve this dialect from, and it fails outright against the pre-fix
// renderer, which emits ants-v1 bullets for every dialect.
TEST(RoadmapRenderPassHeadings, Inv1MigrateThenRenderIsByteStable) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QString first =
        migrateThenRender(tmp, QStringLiteral("a"), QByteArray(kSeed));
    ASSERT_FALSE(first.isEmpty()) << "first render produced nothing";
    EXPECT_TRUE(first.contains(QStringLiteral("#### Pass 43.5 ")))
        << "the render must emit pass headings, not bullets:\n"
        << first.toStdString();
    EXPECT_TRUE(first.contains(QStringLiteral("- **Status**:")))
        << "each pass block carries its Status line";

    const QString second =
        migrateThenRender(tmp, QStringLiteral("b"), first.toUtf8());
    ASSERT_FALSE(second.isEmpty()) << "second render produced nothing";
    EXPECT_EQ(first, second)
        << "migrate->render is not byte-stable; the render is not the "
           "migration's inverse for this dialect";
}

// INV-2 — the read seam serves a migrated pass-headings project from the
// store. This is the gate the item is named for: before, it returned
// "legitimately markdown-served" and the rows were written but never read.
TEST(RoadmapRenderPassHeadings, Inv2ReadSeamServesPassHeadingsFromTheStore) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    bool engaged = false;
    const QString rendered =
        migrateThenRender(tmp, QStringLiteral("g"), QByteArray(kSeed), &engaged);
    ASSERT_FALSE(rendered.isEmpty());
    EXPECT_TRUE(engaged)
        << "a migrated pass-headings project must be served from the store; "
           "write-only is the defect this item was filed about";
}

// INV-3 — the designator survives the trip. The store keeps only the
// synthesised id, so the render recovers "43.5" from "PASS-43-5"; a sub-pass
// must survive too, since that is what distinguishes a parent from its child.
TEST(RoadmapRenderPassHeadings, Inv3DesignatorRecoveryIsTheExactInverse) {
    for (const char *d : {"43.5", "43.5.B", "1.0", "127.99.Alpha2"}) {
        const QString designator = QString::fromLatin1(d);
        const QString id = PassHeadingWrite::passIdFromDesignator(designator);
        ASSERT_FALSE(id.isEmpty()) << d;
        EXPECT_EQ(PassHeadingWrite::designatorFromPassId(id), designator)
            << "round trip lost the designator for " << d;
    }
    // An id of another shape yields nothing rather than a wrong designator.
    EXPECT_TRUE(PassHeadingWrite::designatorFromPassId(
        QStringLiteral("ANTS-4803")).isEmpty());
    EXPECT_TRUE(PassHeadingWrite::designatorFromPassId(QString()).isEmpty());
}

// INV-4 — the dialect switch does not leak. An ants-v1 project still renders
// as bullets, which is the regression a shared render path could cause.
TEST(RoadmapRenderPassHeadings, Inv4AntsV1StillRendersAsBullets) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = tmp.filePath(QStringLiteral("v1"));
    const QByteArray md =
        "<!-- ants-roadmap-format: 1 -->\n"
        "\n"
        "# Demo — Roadmap\n"
        "\n"
        "## Work\n"
        "\n"
        "- \xF0\x9F\x93\x8B [DEMO-0001] **An ordinary bullet item.**\n"
        "  Layman: Ordinary.\n"
        "  Kind: implement.\n"
        "  Source: test.\n";
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"), md));
    auto store = openStore(tmp.filePath(QStringLiteral("v1-store.sqlite")));
    ASSERT_TRUE(store != nullptr);

    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    ASSERT_TRUE(disc.has_value()) << err.toStdString();
    const auto plan = RoadmapMigrate::planFrom(
        *disc, QStringLiteral("Demo"), QStringLiteral("demo"));
    RoadmapMigrateLoad::Options lopts;
    lopts.changedAt   = QStringLiteral("2026-09-03T10:00:00Z");
    lopts.projectRoot = root;
    const auto loaded = RoadmapMigrateLoad::load(*store, plan, lopts);
    ASSERT_TRUE(loaded.ok) << loaded.error.toStdString();
    ASSERT_TRUE(QFile::remove(root + QStringLiteral("/ROADMAP.md")));

    RoadmapRender::Options ropts;
    ropts.liveRoadmapPath = QStringLiteral("ROADMAP.md");
    // dialect left unset — the default must remain the bullet form.
    const auto outcome =
        RoadmapRender::render(*store, loaded.projectId, root, ropts, &err);
    ASSERT_TRUE(outcome.has_value()) << err.toStdString();

    const QString rendered = readAll(root + QStringLiteral("/ROADMAP.md"));
    EXPECT_TRUE(rendered.contains(QStringLiteral("[DEMO-0001]")))
        << "an ants-v1 project must still render as bullets:\n"
        << rendered.toStdString();
    EXPECT_FALSE(rendered.contains(QStringLiteral("#### Pass ")))
        << "the pass-headings emission leaked into the bullet dialect";
}
