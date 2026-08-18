// Feature-conformance test for ANTS-3793 — the read seam. Contract:
// tests/features/roadmap_read_seam/spec.md
//
// Behavioural, against real stores in QTemporaryDirs. INV-2 drives the whole
// realistic path — write markdown, migrate it, render the store back out, parse
// the file that lands — because that is the object the invariant is about: a
// migrated project's ROADMAP.md IS the render's output, so the rendered text is
// what the markdown backend will actually be handed.

#include <gtest/gtest.h>

#include "roadmapmigrate.h"
#include "roadmapmigrateload.h"
#include "roadmapparse.h"
#include "roadmaprender.h"
#include "roadmapsource.h"
#include "roadmapstore.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace {

using RoadmapParse::BulletRecord;
using RoadmapSource::ReadError;

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

// NEVER default-construct RoadmapStore: it resolves defaultPath() — the
// developer's REAL store under XDG_DATA_HOME — so every case here would write
// into it. `Access` is the THIRD parameter, after the history cap, and it is
// named rather than defaulted per § 2.2's sixth rule.
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

// findRoadmaps → planFrom → load, the migration as a consumer would run it.
// Bulk, because RoadmapMigrateLoad::load() refuses an Interactive connection
// outright (ANTS-3765 INV-12).
bool migrateInto(RoadmapStore &store, const QString &root, qint64 *projectId,
                 const QString &exportSlug = QStringLiteral("demo")) {
    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    if (!disc) {
        ADD_FAILURE() << "findRoadmaps: " << err.toStdString();
        return false;
    }
    // The slug is a parameter because Inv1DispatchMarker migrates two projects
    // into ONE store, and project.export_slug is UNIQUE.
    const auto plan = RoadmapMigrate::planFrom(*disc, QStringLiteral("Demo"), exportSlug);
    RoadmapMigrateLoad::Options opts;
    opts.changedAt   = QStringLiteral("2026-08-04T10:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(store, plan, opts);
    if (!out.ok) {
        ADD_FAILURE() << "migration load: " << out.error.toStdString();
        return false;
    }
    *projectId = out.projectId;
    return true;
}

RoadmapStore::ItemWrite item(qint64 projectId, qint64 sectionId, int position,
                             const QString &id, const QString &headline,
                             const QString &status = QStringLiteral("planned")) {
    RoadmapStore::ItemWrite w;
    w.projectId = projectId;
    w.sectionId = sectionId;
    w.position  = position;
    w.id        = id;
    w.headline  = headline;
    w.status    = status;
    w.kind      = QStringLiteral("implement");
    w.source    = QStringLiteral("test");   // item.source is NOT NULL
    // The render's INV-5 gate refuses a project outright when a PUBLIC OPEN
    // item carries no `layman`, so every open item here has one.
    w.layman    = QStringLiteral("A thing.");
    return w;
}

// Every compared field of one record, as text, so a mismatch names the field
// rather than reporting "records differ".
//
// 21 fields — the 23 BulletRecord members minus `firstLine`/`lastLine`, which
// INV-2 declares as differences and this file asserts to be 0 on the store path
// instead of comparing. (§ 2.1.1 counts 22 members over 21 declaration lines;
// it was measured before ANTS-3808's `headlineEnd` landed, so the live figures
// are 23 over 22. The carve-out is unchanged — it is still exactly the two line
// numbers.)
QString describe(const BulletRecord &r) {
    return QStringLiteral(
               "id=[%1] status=[%2] headline=[%3] headlineFull=[%4] kind=[%5] "
               "lanes=[%6] evidence=[%7] layman=[%8] sectionHeading=[%9] "
               "sectionLevel=[%10] sectionSlug=[%11] anchor=[%12] "
               "synthetic=[%13] format=[%14] boldId=[%15] sourceStatus=[%16] "
               "source=[%17] idToken=[%18] passDesignator=[%19] "
               "headlineEnd=[%20]\nbody=[%21]")
        .arg(r.id, r.status, r.headline, r.headlineFull, r.kind,
             r.lanes.join(QLatin1Char('|')), r.evidence.join(QLatin1Char('|')),
             r.layman, r.sectionHeading)
        .arg(r.sectionLevel)
        .arg(r.sectionSlug, r.anchor,
             r.synthetic ? QStringLiteral("true") : QStringLiteral("false"),
             r.format, r.boldId, r.sourceStatus, r.source, r.idToken,
             r.passDesignator)
        .arg(r.headlineEnd)
        .arg(r.body);
}

}  // namespace

// ---------------------------------------------------------------- INV-1 -----

TEST(RoadmapReadSeam, Inv1DispatchMarker) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    // (1) No store file at all. The case that protects every machine that has
    // never migrated anything: markdown, and NOT an error.
    {
        ReadError why = ReadError::TooLarge;   // poisoned, so None must be written
        QString err;
        const auto store = RoadmapSource::storeFor(
            dir.filePath(QStringLiteral("absent.sqlite")), &why, &err);
        EXPECT_EQ(store, nullptr);
        EXPECT_EQ(why, ReadError::None) << "an absent store is not an error";
        EXPECT_TRUE(err.isEmpty()) << err.toStdString();
    }

    // (4) A store file that exists and will not open. Refuse — a corrupted
    // store that quietly falls back to markdown is a store nobody notices is
    // corrupt.
    {
        const QString bad = dir.filePath(QStringLiteral("corrupt.sqlite"));
        ASSERT_TRUE(writeFile(bad, QByteArray("this is not a database at all")));
        ASSERT_TRUE(QFile::setPermissions(bad, QFile::Permissions()));
        ReadError why = ReadError::None;
        QString err;
        const auto store = RoadmapSource::storeFor(bad, &why, &err);
        EXPECT_EQ(store, nullptr);
        EXPECT_EQ(why, ReadError::StoreFailed);
        EXPECT_FALSE(err.isEmpty()) << "a refusal must carry its reason";
        // Let QTemporaryDir clean up.
        QFile::setPermissions(bad, QFile::ReadOwner | QFile::WriteOwner);
    }

    // (2) The same project, queried before and after its migration commits, in
    // ONE process. The marker is resolved per CALL; a cached one would serve
    // markdown for the rest of a session to a project migrated in between.
    const QString root = dir.filePath(QStringLiteral("proj"));
    const QByteArray live =
        "<!-- ants-roadmap-format: 1 -->\n"
        "\n"
        "# Demo — Roadmap\n"
        "\n"
        "## Work\n"
        "\n"
        "- \xF0\x9F\x93\x8B [DEMO-0001] **An open item.**\n"
        "  Layman: A thing.\n"
        "  Kind: implement.\n";
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"), live));
    const QString markdown = readAll(root + QStringLiteral("/ROADMAP.md"));

    auto store = openStore(dir.filePath(QStringLiteral("store.sqlite")),
                           RoadmapStore::Access::Bulk);
    ASSERT_NE(store, nullptr);

    {
        ReadError why = ReadError::TooLarge;
        QString err;
        auto text = RoadmapSource::RoadmapText::fromMemory(markdown);
        const auto before = RoadmapSource::bulletsFor(*store, root, text,
                                                      /*includeArchive=*/false,
                                                      &why, &err);
        EXPECT_FALSE(before.has_value()) << "no project row yet";
        EXPECT_EQ(why, ReadError::None) << "unmigrated is not an error";
        EXPECT_TRUE(err.isEmpty()) << err.toStdString();
    }

    qint64 projectId = 0;
    ASSERT_TRUE(migrateInto(*store, root, &projectId));

    {
        ReadError why = ReadError::TooLarge;
        QString err;
        auto text = RoadmapSource::RoadmapText::fromMemory(markdown);
        const auto after = RoadmapSource::bulletsFor(*store, root, text,
                                                     /*includeArchive=*/false,
                                                     &why, &err);
        ASSERT_TRUE(after.has_value()) << err.toStdString();
        EXPECT_EQ(why, ReadError::None);
        ASSERT_EQ(after->size(), 1);
        EXPECT_EQ(after->front().id, QStringLiteral("DEMO-0001"));
    }

    // The lookup is keyed on the CANONICAL root, so a non-normalised spelling
    // of the same directory must resolve to the same row. A raw-path lookup
    // would miss and report "not migrated" — INV-1's silent fallback, arriving
    // through the one door the invariant does not otherwise watch.
    {
        QString err;
        auto text = RoadmapSource::RoadmapText::fromMemory(markdown);
        const auto viaDotted = RoadmapSource::migratedProject(
            *store, root + QStringLiteral("/."), text, &err);
        ASSERT_TRUE(viaDotted.has_value()) << err.toStdString();
        EXPECT_EQ(*viaDotted, projectId);
    }

    // (5) A migrated project whose roadmap text is empty. detectRoadmapFormat()
    // answers "ants-v1" for input it does not recognise, so the returned
    // dialect cannot separate this from case (3) — `sawSignal` is what does.
    // Refuse, and refuse as SourceUnrecognised: the store is fine and the FILE
    // is not, and the two send the user to different places.
    {
        ReadError why = ReadError::None;
        QString err;
        auto text = RoadmapSource::RoadmapText::fromMemory(QString());
        const auto empty = RoadmapSource::bulletsFor(*store, root, text,
                                                     /*includeArchive=*/false,
                                                     &why, &err);
        EXPECT_FALSE(empty.has_value());
        EXPECT_EQ(why, ReadError::SourceUnrecognised)
            << "an unrecognisable roadmap must not fall back to markdown";
        EXPECT_FALSE(err.isEmpty());
    }

    // (3) A loaded pass-headings project. A store row existing does NOT imply
    // this path can serve it: the migration reads all three dialects and the
    // store backend serves ants-v1 only. Markdown, and NOT a refusal — an
    // earlier draft of INV-1 would have broken every such project.
    {
        const QString passRoot = dir.filePath(QStringLiteral("passproj"));
        const QByteArray passHeadings =
            "# Pass Roadmap\n"
            "\n"
            "#### Pass 1.1 First pass\n"
            "- **Status**: done\n"
            "  Some prose.\n"
            "\n"
            "#### Pass 1.2 Second pass\n"
            "- **Status**: planned\n"
            "  More prose.\n";
        ASSERT_TRUE(writeFile(passRoot + QStringLiteral("/ROADMAP.md"), passHeadings));
        const QString passMarkdown = readAll(passRoot + QStringLiteral("/ROADMAP.md"));
        // Confirm the fixture really is the dialect this case is about, so a
        // pass here cannot come from it having been read as ants-v1.
        bool sawSignal = false;
        ASSERT_EQ(RoadmapParse::detectRoadmapFormat(passMarkdown.split('\n'), &sawSignal),
                  QStringLiteral("pass-headings"));
        ASSERT_TRUE(sawSignal);

        qint64 passProjectId = 0;
        ASSERT_TRUE(migrateInto(*store, passRoot, &passProjectId,
                                QStringLiteral("passdemo")));

        ReadError why = ReadError::TooLarge;
        QString err;
        auto text = RoadmapSource::RoadmapText::fromMemory(passMarkdown);
        const auto served = RoadmapSource::bulletsFor(*store, passRoot, text,
                                                      /*includeArchive=*/false,
                                                      &why, &err);
        EXPECT_FALSE(served.has_value()) << "a foreign dialect is markdown-served";
        EXPECT_EQ(why, ReadError::None) << "recognisably foreign is not a refusal";
        EXPECT_TRUE(err.isEmpty()) << err.toStdString();
    }
}

// ------------------------------------------------------- ANTS-3815 INV-5 ----
// A project whose source_format is '' dispatches EXACTLY as it did at version 1:
// ANTS-3793 § 2.2's table decides on the live file's format alone, and no new
// refusal is reachable.
//
// The stored value is forced back to '' after migration, because the migration
// now writes one — '' is what a project migrated BEFORE the column existed
// carries, and that population is exactly the one this invariant protects.
TEST(RoadmapReadSeam, Ants3815Inv5UnrecordedFormatDispatchesAsBefore) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto store = openStore(dir.filePath(QStringLiteral("store.sqlite")),
                           RoadmapStore::Access::Bulk);
    ASSERT_NE(store, nullptr);
    QString err;

    // (a) an ants-v1 project → the store backend.
    const QString root = dir.filePath(QStringLiteral("proj"));
    const QByteArray live =
        "<!-- ants-roadmap-format: 1 -->\n"
        "\n"
        "# Demo — Roadmap\n"
        "\n"
        "## Work\n"
        "\n"
        "- \xF0\x9F\x93\x8B [DEMO-0001] **An open item.**\n"
        "  Layman: A thing.\n"
        "  Kind: implement.\n";
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"), live));
    const QString markdown = readAll(root + QStringLiteral("/ROADMAP.md"));

    qint64 projectId = 0;
    ASSERT_TRUE(migrateInto(*store, root, &projectId));
    ASSERT_TRUE(store->setProjectSourceFormat(projectId, QString::fromUtf8(""), &err))
        << err.toStdString();

    {
        ReadError why = ReadError::TooLarge;
        err.clear();
        auto text = RoadmapSource::RoadmapText::fromMemory(markdown);
        const auto served = RoadmapSource::migratedProject(*store, root, text,
                                                           &err, &why);
        ASSERT_TRUE(served.has_value()) << err.toStdString();
        EXPECT_EQ(*served, projectId);
        EXPECT_EQ(why, ReadError::None);
    }

    // (b) the sawSignal == false refusal still fires, and still as
    // SourceUnrecognised rather than as the new disagreement message.
    {
        ReadError why = ReadError::None;
        err.clear();
        auto text = RoadmapSource::RoadmapText::fromMemory(QString());
        const auto refused = RoadmapSource::migratedProject(*store, root, text,
                                                            &err, &why);
        EXPECT_FALSE(refused.has_value());
        EXPECT_EQ(why, ReadError::SourceUnrecognised);
        EXPECT_TRUE(err.contains(QStringLiteral("unrecognisable")))
            << "an unrecorded format must not produce the drift message: "
            << err.toStdString();
    }

    // (c) a pass-headings project is still markdown-served, not refused. This is
    // the case the invariant exists for: '' matches no dialect a file can
    // produce, so treating the stored value as authoritative before it is known
    // to be SET turns every pre-bump project into a refusal.
    {
        const QString passRoot = dir.filePath(QStringLiteral("passproj"));
        const QByteArray passHeadings =
            "# Pass Roadmap\n"
            "\n"
            "#### Pass 1.1 First pass\n"
            "- **Status**: done\n"
            "  Some prose.\n"
            "\n"
            "#### Pass 1.2 Second pass\n"
            "- **Status**: planned\n"
            "  More prose.\n";
        ASSERT_TRUE(writeFile(passRoot + QStringLiteral("/ROADMAP.md"), passHeadings));
        const QString passMarkdown = readAll(passRoot + QStringLiteral("/ROADMAP.md"));

        qint64 passId = 0;
        ASSERT_TRUE(migrateInto(*store, passRoot, &passId, QStringLiteral("passdemo")));
        ASSERT_TRUE(store->setProjectSourceFormat(passId, QString::fromUtf8(""), &err))
            << err.toStdString();

        ReadError why = ReadError::TooLarge;
        err.clear();
        auto text = RoadmapSource::RoadmapText::fromMemory(passMarkdown);
        const auto served = RoadmapSource::migratedProject(*store, passRoot,
                                                           text, &err, &why);
        EXPECT_FALSE(served.has_value()) << "a foreign dialect is markdown-served";
        EXPECT_EQ(why, ReadError::None) << "recognisably foreign is not a refusal";
        EXPECT_TRUE(err.isEmpty()) << err.toStdString();
    }
}

// ------------------------------------------------------- ANTS-3815 INV-6 ----
// A SET source_format that disagrees with the live file's detected format
// refuses, and serves NEITHER backend.
//
// This is the outcome one witness could not reach. ANTS-3793 already refused "a
// file that no longer looks like what the store says it is", but at version 1
// the only disagreement a lone witness could have with itself was an
// UNRECOGNISABLE file; a roadmap rewritten from ants-v1 into valid GFM is the
// same class of drift and took the markdown branch silently.
TEST(RoadmapReadSeam, Ants3815Inv6StoredFormatDisagreeingWithTheFileRefuses) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto store = openStore(dir.filePath(QStringLiteral("store.sqlite")),
                           RoadmapStore::Access::Bulk);
    ASSERT_NE(store, nullptr);
    QString err;

    const QString root = dir.filePath(QStringLiteral("proj"));
    const QByteArray live =
        "<!-- ants-roadmap-format: 1 -->\n"
        "\n"
        "# Demo — Roadmap\n"
        "\n"
        "## Work\n"
        "\n"
        "- \xF0\x9F\x93\x8B [DEMO-0001] **An open item.**\n"
        "  Layman: A thing.\n"
        "  Kind: implement.\n";
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"), live));

    qint64 projectId = 0;
    ASSERT_TRUE(migrateInto(*store, root, &projectId));

    // The migration recorded the dialect it read — the precondition this case
    // rests on, asserted rather than assumed.
    {
        const auto row = store->readProject(projectId, &err);
        ASSERT_TRUE(row.has_value()) << err.toStdString();
        ASSERT_EQ(row->sourceFormat.toStdString(), std::string("ants-v1"));
    }

    // The roadmap is rewritten into a valid GFM task list — recognisable, so
    // sawSignal is set and the version-1 gate would have served it markdown.
    const QByteArray gfm =
        "# Demo — Roadmap\n"
        "\n"
        "## Work\n"
        "\n"
        "- [ ] An open item\n"
        "- [x] A closed one\n";
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"), gfm));
    const QString rewritten = readAll(root + QStringLiteral("/ROADMAP.md"));
    {
        bool sawSignal = false;
        ASSERT_EQ(RoadmapParse::detectRoadmapFormat(rewritten.split('\n'), &sawSignal),
                  QStringLiteral("github-task-list"));
        ASSERT_TRUE(sawSignal) << "the fixture must be RECOGNISABLY foreign, or this "
                                  "case collapses into the sawSignal refusal";
    }

    ReadError why = ReadError::None;
    err.clear();
    auto driftText = RoadmapSource::RoadmapText::fromMemory(rewritten);
    const auto served = RoadmapSource::migratedProject(*store, root, driftText,
                                                       &err, &why);
    EXPECT_FALSE(served.has_value())
        << "INV-6: breaks when the disagreement falls through to the markdown "
           "backend — ANTS-3793 INV-1's silent fallback arriving through the one "
           "door that spec could not close with a single witness";
    EXPECT_EQ(why, ReadError::SourceUnrecognised);
    ASSERT_FALSE(err.isEmpty()) << "a refusal must carry its reason";
    EXPECT_TRUE(err.contains(QStringLiteral("ants-v1")) &&
                err.contains(QStringLiteral("github-task-list")))
        << "the message must name BOTH formats — SourceUnrecognised now carries "
           "two causes with different remedies: " << err.toStdString();

    // And no records come back through the seam either.
    why = ReadError::None;
    err.clear();
    auto recordsText = RoadmapSource::RoadmapText::fromMemory(rewritten);
    const auto records = RoadmapSource::bulletsFor(*store, root, recordsText,
                                                   /*includeArchive=*/false, &why, &err);
    EXPECT_FALSE(records.has_value()) << "neither backend may serve a drifted project";
    EXPECT_EQ(why, ReadError::SourceUnrecognised);

    // The remedy the refusal message names, exercised. Without this leg the
    // message advertises a route back that nothing tests — and a
    // registerProject() that refused an already-registered canonical root would
    // break it silently, since it is get-or-create and nothing here asserts so.
    qint64 again = 0;
    ASSERT_TRUE(migrateInto(*store, root, &again));
    EXPECT_EQ(again, projectId) << "a re-migration must reuse the project row";
    {
        const auto row = store->readProject(projectId, &err);
        ASSERT_TRUE(row.has_value()) << err.toStdString();
        EXPECT_EQ(row->sourceFormat.toStdString(), std::string("github-task-list"))
            << "re-running the migration must rewrite source_format";
    }
    why = ReadError::TooLarge;
    err.clear();
    auto afterText = RoadmapSource::RoadmapText::fromMemory(rewritten);
    const auto after = RoadmapSource::migratedProject(*store, root, afterText,
                                                      &err, &why);
    EXPECT_FALSE(after.has_value())
        << "a GFM project is markdown-served, not store-served";
    EXPECT_EQ(why, ReadError::None) << "the refusal must be gone after re-migration";
    EXPECT_TRUE(err.isEmpty()) << err.toStdString();
}

// ---------------------------------------------------------------- INV-2 -----

TEST(RoadmapReadSeam, Inv2BackendsAgree) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = dir.filePath(QStringLiteral("proj"));

    // The fixture carries the shapes § 2.1.1's table says a naive
    // implementation gets wrong: a preamble bullet under the level-0 synthetic
    // root (which emits no heading, so its section fields are EMPTY), a
    // duplicated heading title (the parser's slugger is stateful, so the second
    // gets `-2`), an over-long headline (truncated at 120 then `…` appended),
    // and a body carrying trailer keys inline.
    const QString longHeadline =
        QStringLiteral("A headline written deliberately past the hundred and twenty "
                       "character display limit so the truncation and its ellipsis "
                       "are both exercised here.");
    ASSERT_GT(longHeadline.size(), 120);

    QByteArray liveMd =
        "<!-- ants-roadmap-format: 1 -->\n"
        "\n"
        "# Demo — Roadmap\n"
        "\n"
        "- \xE2\x9C\x85 [DEMO-0001] **A preamble item above every heading.**\n"
        "  Layman: Sits under the synthetic root.\n"
        "  Kind: implement.\n"
        "  Source: test.\n"
        "\n"
        "## Work\n"
        "\n"
        "- \xF0\x9F\x93\x8B [DEMO-0002] **An ordinary open item.**\n"
        "  Layman: Ordinary.\n"
        "  Kind: implement.\n"
        "  Lanes: packaging, docs.\n"
        "\n"
        "- \xF0\x9F\x9A\xA7 [DEMO-0003] **";
    liveMd += longHeadline.toUtf8();
    liveMd +=
        "**\n"
        "  Layman: Long.\n"
        "  Kind: fix.\n"
        "\n"
        "- \xF0\x9F\x92\xAD [DEMO-0004] **A fourth item.**\n"
        "  Layman: Considered.\n"
        "  Kind: research.\n";
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"), liveMd));

    auto bulk = openStore(dir.filePath(QStringLiteral("store.sqlite")),
                          RoadmapStore::Access::Bulk);
    ASSERT_NE(bulk, nullptr);
    qint64 projectId = 0;
    ASSERT_TRUE(migrateInto(*bulk, root, &projectId));

    // Two shapes migration cannot produce, written directly so INV-2 covers
    // them anyway.
    {
        QString err;
        int maxSectionPos = 0;
        const auto sections = bulk->listSections(projectId, &err);
        ASSERT_TRUE(sections.has_value()) << err.toStdString();
        for (const RoadmapStore::SectionRow &s : *sections)
            maxSectionPos = std::max(maxSectionPos, s.position);

        // (a) A SECOND SECTION WITH THE SAME TITLE. Migration cannot make one:
        // it slugs headings itself, so two `## Work` headings in one source
        // file resolve to a single section under UNIQUE (project_id, slug).
        // Written with a distinct slug and the same title, this is the shape
        // that pins the parser's slugger being STATEFUL — the render emits
        // `## Work` twice, so the second section's bullets must come back with
        // slug `work-2`, which a stateless per-section slug gets wrong.
        const auto again = bulk->addSection(projectId, QStringLiteral("work-again"),
                                            QStringLiteral("Work"), 2,
                                            maxSectionPos + 1, std::nullopt, &err);
        ASSERT_TRUE(again.has_value()) << err.toStdString();

        // (b) An item whose `id` column is EMPTY. It renders without the
        // bracket, and parseBullets() then takes id/idToken/boldId off an
        // id-shaped bold headline word instead — the commonest way a field
        // table copied from the columns fails INV-2. EMPTY and not null:
        // item.id is NOT NULL and a default-constructed QString binds as SQL
        // NULL, the same trap section.slug carries.
        auto w = item(projectId, *again, 0, QString::fromUtf8(""),
                      QStringLiteral("Idless"), QStringLiteral("shipped"));
        w.layman.clear();   // shipped, so INV-5's open-item gate does not apply
        ASSERT_TRUE(bulk->putItem(w, &err).has_value()) << err.toStdString();
    }

    // Render the store back out. This file — not the fixture above — is what
    // the markdown backend will actually be handed after cutover.
    QString err;
    RoadmapRender::Options opts;
    opts.liveRoadmapPath = QStringLiteral("ROADMAP.md");
    const auto outcome = RoadmapRender::render(*bulk, projectId, root, opts, &err);
    ASSERT_TRUE(outcome.has_value()) << err.toStdString();
    ASSERT_TRUE(outcome->gateFailures.isEmpty())
        << outcome->gateFailures.join(QStringLiteral(", ")).toStdString();
    ASSERT_TRUE(outcome->committed);

    const QString rendered = readAll(root + QStringLiteral("/ROADMAP.md"));
    ASSERT_FALSE(rendered.isEmpty());

    const QVector<BulletRecord> fromMarkdown = RoadmapParse::parseBullets(rendered);

    // A second, INTERACTIVE connection — the profile a consumer opens with.
    auto interactive = openStore(dir.filePath(QStringLiteral("store.sqlite")),
                                 RoadmapStore::Access::Interactive);
    ASSERT_NE(interactive, nullptr);
    ReadError why = ReadError::TooLarge;
    const auto fromStore = RoadmapSource::bulletsFromStore(
        *interactive, projectId, /*includeArchive=*/false, &why, &err);
    ASSERT_TRUE(fromStore.has_value()) << err.toStdString();
    EXPECT_EQ(why, ReadError::None);

    ASSERT_EQ(fromStore->size(), fromMarkdown.size())
        << "the two backends disagree on how many bullets this project has";
    ASSERT_EQ(fromStore->size(), 5);

    for (int i = 0; i < fromStore->size(); ++i) {
        // Same INDEX as well as same content: document order is half of what
        // INV-2 asserts, so comparing sorted sets would drop it.
        EXPECT_EQ(describe(fromStore->at(i)), describe(fromMarkdown.at(i)))
            << "record " << i << " differs between the backends";
        // The two declared differences, asserted rather than compared.
        EXPECT_EQ(fromStore->at(i).firstLine, 0);
        EXPECT_EQ(fromStore->at(i).lastLine, 0);
    }

    // The fixture's own point, restated as assertions so a change that made
    // BOTH backends wrong in the same way still reddens — the loop above can
    // only see them disagreeing.
    const auto find = [&](const QString &id) -> BulletRecord {
        for (const BulletRecord &r : *fromStore)
            if (r.id == id)
                return r;
        ADD_FAILURE() << "no record with id " << id.toStdString();
        return BulletRecord{};
    };

    const BulletRecord preamble = find(QStringLiteral("DEMO-0001"));
    EXPECT_TRUE(preamble.sectionHeading.isEmpty())
        << "a level-0 synthetic root emits no heading, so its bullets have none";
    EXPECT_TRUE(preamble.sectionSlug.isEmpty());
    EXPECT_EQ(preamble.sectionLevel, 0);

    const BulletRecord ordinary = find(QStringLiteral("DEMO-0002"));
    EXPECT_EQ(ordinary.sectionSlug, QStringLiteral("work"));
    EXPECT_EQ(ordinary.sectionLevel, 2);
    EXPECT_EQ(ordinary.status, QStringLiteral("📋"))
        << "the store holds a lifecycle WORD; the record holds the emoji";
    EXPECT_EQ(ordinary.lanes, QStringList({QStringLiteral("packaging"),
                                           QStringLiteral("docs")}));

    const BulletRecord longOne = find(QStringLiteral("DEMO-0003"));
    EXPECT_EQ(longOne.headline.size(), 121)
        << "120 characters plus the appended ellipsis";
    EXPECT_TRUE(longOne.headline.endsWith(QStringLiteral("…")));
    EXPECT_EQ(longOne.headlineFull, longHeadline);

    // The id-less item, in the second section titled `Work`: no bracket is
    // rendered, so id/idToken/boldId all come off the head line rather than
    // from the (empty) id column — and its slug carries the stateful slugger's
    // `-2` suffix.
    const BulletRecord idless = find(QStringLiteral("Idless"));
    EXPECT_EQ(idless.boldId, QStringLiteral("Idless"));
    // BOTH, per § 2.1.1's last row: with no bracket to capture, rxLeadToken
    // misses and the leading slot falls through to the bold token.
    EXPECT_EQ(idless.idToken, QStringLiteral("Idless"));
    EXPECT_EQ(idless.sectionHeading, QStringLiteral("Work"));
    EXPECT_EQ(idless.sectionSlug, QStringLiteral("work-2"))
        << "uniqueSlug() is stateful across the whole walk";
}

TEST(RoadmapReadSeam, Inv2Membership) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    // Populated DIRECTLY and not by migration, because migration cannot produce
    // any of the three shapes this case is about: `visibility` defaults to
    // public and no migration path writes it, statusFromMarker() cannot produce
    // `dropped`, and every migrated item is filed.
    auto store = openStore(dir.filePath(QStringLiteral("store.sqlite")),
                           RoadmapStore::Access::Interactive);
    ASSERT_NE(store, nullptr);
    QString err;
    const auto projectId = store->registerProject(dir.path(), QStringLiteral("Demo"),
                                                  QStringLiteral("demo"), &err);
    ASSERT_TRUE(projectId.has_value()) << err.toStdString();
    // The synthetic root's slug and title are EMPTY, not null: a default-
    // constructed QString binds as SQL NULL and section.slug is NOT NULL.
    ASSERT_TRUE(store->addSection(*projectId, QString::fromUtf8(""), QString::fromUtf8(""),
                                  0, 0, std::nullopt, &err).has_value())
        << err.toStdString();
    const auto work = store->addSection(*projectId, QStringLiteral("work"),
                                        QStringLiteral("Work"), 2, 1, std::nullopt, &err);
    ASSERT_TRUE(work.has_value()) << err.toStdString();

    ASSERT_TRUE(store->putItem(item(*projectId, *work, 0, QStringLiteral("DEMO-0001"),
                                    QStringLiteral("An ordinary item.")), &err)
                    .has_value()) << err.toStdString();

    auto internal = item(*projectId, *work, 1, QStringLiteral("DEMO-0002"),
                         QStringLiteral("An internal item."));
    internal.visibility = QStringLiteral("internal");
    ASSERT_TRUE(store->putItem(internal, &err).has_value()) << err.toStdString();

    auto dropped = item(*projectId, *work, 2, QStringLiteral("DEMO-0003"),
                        QStringLiteral("A dropped item."), QStringLiteral("dropped"));
    ASSERT_TRUE(store->putItem(dropped, &err).has_value()) << err.toStdString();

    ASSERT_TRUE(store->putItem(item(*projectId, *work, 3, QStringLiteral("DEMO-0004"),
                                    QStringLiteral("A second ordinary item.")), &err)
                    .has_value()) << err.toStdString();

    // Unfiled: putItem() files by its element row, so the item is written and
    // then unfiled — the state ANTS-3758's INV-4 refuses the whole render over,
    // which is exactly why a QUERY must still show it.
    const auto unfiledPk = store->putItem(item(*projectId, *work, 4,
                                               QStringLiteral("DEMO-0005"),
                                               QStringLiteral("An unfiled item.")), &err);
    ASSERT_TRUE(unfiledPk.has_value()) << err.toStdString();
    ASSERT_TRUE(store->unfileItem(*unfiledPk, &err)) << err.toStdString();

    ReadError why = ReadError::TooLarge;
    const auto records = RoadmapSource::bulletsFromStore(
        *store, *projectId, /*includeArchive=*/false, &why, &err);
    ASSERT_TRUE(records.has_value()) << err.toStdString();
    EXPECT_EQ(why, ReadError::None);

    QStringList ids;
    for (const BulletRecord &r : *records)
        ids.append(r.id);

    EXPECT_FALSE(ids.contains(QStringLiteral("DEMO-0003")))
        << "a dropped item renders with no status marker, so it is no record";
    EXPECT_TRUE(ids.contains(QStringLiteral("DEMO-0002")))
        << "BulletRecord has no visibility field; filtering here would give "
           "roadmap_query a concept it has never had";
    ASSERT_TRUE(ids.contains(QStringLiteral("DEMO-0005")))
        << "hiding an unfiled item hides the fault from the only read likely "
           "to surface it";
    EXPECT_EQ(ids, QStringList({QStringLiteral("DEMO-0001"), QStringLiteral("DEMO-0002"),
                                QStringLiteral("DEMO-0004"), QStringLiteral("DEMO-0005")}))
        << "unfiled items come last, after every filed one";

    const BulletRecord &unfiled = records->back();
    EXPECT_EQ(unfiled.id, QStringLiteral("DEMO-0005"));
    EXPECT_TRUE(unfiled.sectionSlug.isEmpty());
    EXPECT_TRUE(unfiled.sectionHeading.isEmpty());
    EXPECT_EQ(unfiled.sectionLevel, 0);
}

// ---------------------------------------------------------------- INV-3 -----

namespace {

// A project of `count` filed items, in one transaction. `ItemRef` carries a
// headline and four scalars rather than a body, which is what makes the
// pre-read count gate cheap.
qint64 makeSizedProject(RoadmapStore &store, const QString &root, int count) {
    QString err;
    const auto projectId = store.registerProject(root, QStringLiteral("Big"),
                                                 QStringLiteral("big"), &err);
    if (!projectId) {
        ADD_FAILURE() << "registerProject: " << err.toStdString();
        return 0;
    }
    const auto work = store.addSection(*projectId, QStringLiteral("work"),
                                       QStringLiteral("Work"), 2, 0, std::nullopt, &err);
    if (!work) {
        ADD_FAILURE() << "addSection: " << err.toStdString();
        return 0;
    }
    if (!store.begin(&err)) {
        ADD_FAILURE() << "begin: " << err.toStdString();
        return 0;
    }
    for (int i = 0; i < count; ++i) {
        auto w = item(*projectId, *work, i,
                      QStringLiteral("BIG-%1").arg(i, 5, 10, QLatin1Char('0')),
                      QStringLiteral("Item number %1.").arg(i));
        if (!store.putItem(w, &err)) {
            ADD_FAILURE() << "putItem " << i << ": " << err.toStdString();
            store.rollback();
            return 0;
        }
    }
    if (!store.commit(&err)) {
        ADD_FAILURE() << "commit: " << err.toStdString();
        return 0;
    }
    return *projectId;
}

}  // namespace

TEST(RoadmapReadSeam, Inv3Ceiling) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto store = openStore(dir.filePath(QStringLiteral("store.sqlite")),
                           RoadmapStore::Access::Bulk);
    ASSERT_NE(store, nullptr);

    // Exactly at the ceiling — accepted, which is what pins `>` rather than
    // `>=`. The read materialises every body, so this is also the case that
    // would red if the gate were tested after the bodies were already resident.
    const qint64 projectId =
        makeSizedProject(*store, dir.path(), RoadmapSource::kItemCeiling);
    ASSERT_NE(projectId, 0);

    QString err;
    ReadError why = ReadError::TooLarge;
    const auto atCeiling = RoadmapSource::bulletsFromStore(
        *store, projectId, /*includeArchive=*/false, &why, &err);
    ASSERT_TRUE(atCeiling.has_value()) << err.toStdString();
    EXPECT_EQ(why, ReadError::None);
    EXPECT_EQ(atCeiling->size(), RoadmapSource::kItemCeiling);

    // One item past it — refused, and refused as TooLarge rather than degraded
    // or truncated: a partial record set is indistinguishable to every consumer
    // from a project with fewer bullets.
    const auto work = store->findSection(projectId, QStringLiteral("work"), &err);
    ASSERT_TRUE(work.has_value()) << err.toStdString();
    ASSERT_TRUE(store->putItem(item(projectId, *work, RoadmapSource::kItemCeiling,
                                    QStringLiteral("BIG-OVER"),
                                    QStringLiteral("One too many.")), &err)
                    .has_value()) << err.toStdString();

    why = ReadError::None;
    err.clear();
    const auto overCeiling = RoadmapSource::bulletsFromStore(
        *store, projectId, /*includeArchive=*/false, &why, &err);
    EXPECT_FALSE(overCeiling.has_value());
    EXPECT_EQ(why, ReadError::TooLarge);
    EXPECT_FALSE(err.isEmpty()) << "a refusal must carry its reason";
}

// The `perf` label that keeps the benchmark below out of the default suite is
// applied at ctest time, in tests/slow_test_timeouts.cmake — PRE_TEST discovery
// leaves no test for CMakeLists.txt to name at configure time. Naming a test
// that does not exist there is SILENTLY IGNORED, so a rename of the case below
// would quietly put a timing assertion back into the pre-push gate. This is the
// guard for that, in the shape ANTS-3658 already used for its TIMEOUT override.
TEST(RoadmapReadSeam, Ants3793LatencyCaseIsPerfLabelled) {
    QFile f(QStringLiteral(ANTS_SLOW_TEST_TIMEOUTS_CMAKE));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly)) << "cannot read the ctest property file";
    const QString text = QString::fromUtf8(f.readAll());
    ASSERT_TRUE(text.contains(QStringLiteral("RoadmapReadSeam.Inv3Latency")))
        << "the p95 benchmark's perf label names a case that no longer exists, "
           "so it is now running unlabelled in the default suite";
}

// ASan instruments every load and store, so under it this stops measuring the
// read at all — see the GTEST_SKIP below. Same detection ANTS-3761's INV-12
// case uses, and the same two-compiler dance (GCC defines the macro; Clang
// answers __has_feature).
#if defined(__SANITIZE_ADDRESS__)
#  define ANTS_TEST_ASAN 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define ANTS_TEST_ASAN 1
#  endif
#endif

// INV-3's p95, and the budget is a WALL-CLOCK one, which makes an unsanitized
// build a precondition of the instrument rather than a tolerance to widen.
// Measured in ci.yml's own run of 6f78990f: build-asan reported p95 217.9 ms
// against the 50 ms budget while build-test — the SAME commit, the SAME runner
// image, the same 1,839-item project — passed. A 4.4x spread between two jobs
// that differ only in `-fsanitize=address` is the sanitizer's shadow-memory and
// per-access checks being timed, not a read that got slower.
//
// Relaxing the number instead would be the wrong repair twice over: 218 ms is
// above even the markdown path this seam replaces (33.2 ms for a 3 MB file, § 4),
// so a budget wide enough to pass under ASan would no longer fail for a genuine
// N+1 regression — which is the one thing § 4 built this case to catch, and
// which it already caught once (101.5 ms, remedied by readItems()).
TEST(RoadmapReadSeam, Inv3Latency) {
#ifdef ANTS_TEST_ASAN
    GTEST_SKIP() << "INV-3's p95 is unmeasurable under ASan: instrumented "
                    "loads and stores dominate the timing. Enforced by the "
                    "Release build (ci.yml's build-test job, ctest --preset=perf "
                    "locally).";
#endif
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto store = openStore(dir.filePath(QStringLiteral("store.sqlite")),
                           RoadmapStore::Access::Bulk);
    ASSERT_NE(store, nullptr);

    // Corpus-sized: this project's own live roadmap carried 1,839 top-level
    // emoji bullets when § 4 measured it. The figure moves with every append;
    // the budget is insensitive to the last digits.
    constexpr int kCorpusItems = 1839;
    const qint64 projectId = makeSizedProject(*store, dir.path(), kCorpusItems);
    ASSERT_NE(projectId, 0);

    auto interactive = openStore(dir.filePath(QStringLiteral("store.sqlite")),
                                 RoadmapStore::Access::Interactive);
    ASSERT_NE(interactive, nullptr);

    QString err;
    ReadError why = ReadError::None;
    // Warm: the budget assumes § 2.2's process-owned connection, so the page
    // cache is warm by the time a consumer's second call arrives.
    for (int i = 0; i < 3; ++i) {
        const auto warm = RoadmapSource::bulletsFromStore(
            *interactive, projectId, /*includeArchive=*/false, &why, &err);
        ASSERT_TRUE(warm.has_value()) << err.toStdString();
    }

    constexpr int kRuns = 20;
    std::vector<qint64> elapsedUs;
    elapsedUs.reserve(kRuns);
    for (int i = 0; i < kRuns; ++i) {
        QElapsedTimer t;
        t.start();
        const auto records = RoadmapSource::bulletsFromStore(
            *interactive, projectId, /*includeArchive=*/false, &why, &err);
        elapsedUs.push_back(t.nsecsElapsed() / 1000);
        ASSERT_TRUE(records.has_value()) << err.toStdString();
        ASSERT_EQ(records->size(), kCorpusItems);
    }

    std::sort(elapsedUs.begin(), elapsedUs.end());
    const qint64 p95 = elapsedUs.at(size_t(double(kRuns) * 0.95) - 1);
    EXPECT_LT(p95, 50 * 1000)
        << "p95 " << (double(p95) / 1000.0) << " ms over " << kRuns
        << " warm reads of a " << kCorpusItems << "-item project. § 4's remedy "
           "if this reds is a batched RoadmapStore::readItems() (ANTS-3816) — "
           "not a cache and not a relaxed budget.";
}

// ============================================================================
// ANTS-3863 — dispatch before reading.
//
// Contract: docs/specs/ANTS-3863-pre-read-dispatch.md. Every case here is
// prefixed `Ants3863` because the existing `Inv2…`/`Inv3…` cases above carry
// ANTS-3793's invariant numbering, which collides with this document's INV-2
// and INV-3 in a file now serving three specs (§ 7).
// ============================================================================

namespace {

using RoadmapSource::RoadmapText;

QByteArray readAllBytes(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QByteArray();
    return f.readAll();
}

// The byte offset at which `bytes`' kDetectorLineCap'th non-blank line ends,
// and the length of the line after it. INV-1's bound is DERIVED from the
// fixture rather than chosen: the reader may overshoot to finish the line it is
// on, and may not overshoot further.
struct PrefixBound {
    qint64 end = 0;        // offset just past the 300th non-blank line
    qint64 nextLine = 0;   // length of the line that follows it
    bool   reached = false;
};

PrefixBound prefixBoundOf(const QByteArray &bytes) {
    PrefixBound b;
    int nonBlank = 0;
    qsizetype pos = 0;
    while (pos < bytes.size()) {
        const qsizetype nl = bytes.indexOf('\n', pos);
        const qsizetype lineEnd = (nl < 0) ? bytes.size() : nl + 1;
        if (!QString::fromUtf8(bytes.mid(pos, lineEnd - pos)).trimmed().isEmpty())
            ++nonBlank;
        pos = lineEnd;
        if (nonBlank >= RoadmapSource::kDetectorLineCap) {
            b.end     = pos;
            b.reached = true;
            const qsizetype nl2 = bytes.indexOf('\n', pos);
            b.nextLine = ((nl2 < 0) ? bytes.size() : nl2 + 1) - pos;
            break;
        }
    }
    if (!b.reached)
        b.end = bytes.size();
    return b;
}

// A rendered-looking ants-v1 roadmap head, then `bullets` further valid items.
QByteArray antsV1Roadmap(int bullets) {
    QByteArray out =
        "<!-- ants-roadmap-format: 1 -->\n"
        "\n"
        "# Demo — Roadmap\n"
        "\n"
        "## Work\n"
        "\n";
    for (int i = 1; i <= bullets; ++i) {
        out += "- \xF0\x9F\x93\x8B [DEMO-"
             + QByteArray::number(i).rightJustified(4, '0')
             + "] **Item " + QByteArray::number(i) + ".**\n"
               "  Layman: A thing.\n"
               "  Kind: implement.\n";
    }
    return out;
}

}  // namespace

// --------------------------------------------------------- ANTS-3863 INV-1 --
// A dispatch against a MIGRATED project through a FILE-BACKED RoadmapText reads
// at most kDetectorLineCap non-blank lines or kDetectorByteCap bytes — never
// the body.
//
// perf-labelled (§ 7): the fixture has to be multi-megabyte for the assertion
// to mean anything, and that is also what makes it too slow for `fast`.
TEST(RoadmapReadSeam, Ants3863Inv1DispatchReadsOnlyTheDetectorWindow) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = dir.filePath(QStringLiteral("proj"));
    const QString rm   = root + QStringLiteral("/ROADMAP.md");

    ASSERT_TRUE(writeFile(rm, antsV1Roadmap(4)));
    auto store = openStore(dir.filePath(QStringLiteral("store.sqlite")),
                           RoadmapStore::Access::Bulk);
    ASSERT_NE(store, nullptr);
    qint64 projectId = 0;
    ASSERT_TRUE(migrateInto(*store, root, &projectId));

    // A multi-megabyte tail of VALID ants-v1 bullets: the file still detects as
    // ants-v1 and still agrees with the stored source_format, so the dispatch
    // must succeed — the only question is how much of it was read.
    QByteArray big = antsV1Roadmap(4);
    while (big.size() < 3 * 1024 * 1024)
        big += antsV1Roadmap(200);
    ASSERT_TRUE(writeFile(rm, big));

    const qint64 fileSize = QFileInfo(rm).size();
    const PrefixBound bound = prefixBoundOf(readAllBytes(rm));
    ASSERT_TRUE(bound.reached)
        << "the fixture must be long enough to reach the line cap, or this "
           "case bounds nothing";

    QString err;
    ReadError why = ReadError::TooLarge;
    auto text = RoadmapText::fromFile(rm);
    const auto served =
        RoadmapSource::migratedProject(*store, root, text, &err, &why);

    ASSERT_TRUE(served.has_value()) << err.toStdString();
    EXPECT_EQ(*served, projectId);
    EXPECT_EQ(why, ReadError::None);

    EXPECT_LE(text.bytesRead(), bound.end + bound.nextLine)
        << "the dispatch read past the 300th non-blank line (ends at byte "
        << bound.end << ") by more than the line it was on";
    // Cap-independent, and the leg that fails loudly if the LINE cap is lost.
    EXPECT_LT(text.bytesRead(), fileSize / 100)
        << "read " << text.bytesRead() << " of " << fileSize << " bytes";
}

// The BYTE cap's own leg: a file where the non-blank cap is never reached, so
// the line cap bounds nothing at all. Sized ABOVE kDetectorByteCap on purpose —
// a 1,000,000-byte fixture is under a 1 MiB cap and would pass whether or not
// the cap existed, which is the one shape a cap test must not have.
TEST(RoadmapReadSeam, Ants3863Inv1ByteCapBoundsAnAllBlankRoadmap) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = dir.filePath(QStringLiteral("proj"));
    const QString rm   = root + QStringLiteral("/ROADMAP.md");

    ASSERT_TRUE(writeFile(rm, antsV1Roadmap(4)));
    auto store = openStore(dir.filePath(QStringLiteral("store.sqlite")),
                           RoadmapStore::Access::Bulk);
    ASSERT_NE(store, nullptr);
    qint64 projectId = 0;
    ASSERT_TRUE(migrateInto(*store, root, &projectId));

    ASSERT_TRUE(writeFile(rm, QByteArray(2 * 1024 * 1024, '\n')));
    const qint64 fileSize = QFileInfo(rm).size();
    ASSERT_GT(fileSize, RoadmapSource::kDetectorByteCap);

    QString err;
    ReadError why = ReadError::None;
    auto text = RoadmapText::fromFile(rm);
    const auto served =
        RoadmapSource::migratedProject(*store, root, text, &err, &why);

    // It RETURNS — a blank file classifies as unrecognisable rather than
    // hanging or draining the file.
    EXPECT_FALSE(served.has_value());
    EXPECT_EQ(why, ReadError::SourceUnrecognised);

    // +1 byte: the reader looks one byte past the cap so detectionPrefixOf()
    // can see that the crossing line crosses (see readHead()'s comment) — that
    // lookahead is what keeps the two producers in agreement for INV-2.
    EXPECT_LE(text.bytesRead(), RoadmapSource::kDetectorByteCap + 1);
    // The assertion that gives the leg something to be wrong about: without a
    // byte cap this is the whole 2 MiB.
    EXPECT_LT(text.bytesRead(), fileSize);
}

// --------------------------------------------------------- ANTS-3863 INV-2 --
// The bounded file reader and the in-memory helper produce the SAME
// QStringList, so swapping one for the other cannot change a classification.
//
// This is READER equivalence, not prefix-versus-whole-file equivalence: both
// sides are capped, so on the late-signal fixture both correctly MISS the
// signal and agree. That a capped prefix classifies a whole file the same way
// is detectionPrefixOf()'s own pre-existing property, untouched here.
TEST(RoadmapReadSeam, Ants3863Inv2BothProducersAgree) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    struct Fixture { const char *name; QByteArray body; };
    std::vector<Fixture> fixtures;

    fixtures.push_back({"ants-v1", antsV1Roadmap(6)});

    fixtures.push_back({"github-task-list",
        QByteArray("# Demo — Roadmap\n\n## Work\n\n"
                   "- [ ] An open item\n"
                   "- [x] A done item\n"
                   "- [ ] Another open item\n")});

    fixtures.push_back({"pass-headings",
        QByteArray("# Demo — Roadmap\n\n"
                   "#### Pass 1.1 First\n- **Status**: done\n\n"
                   "#### Pass 1.2 Second\n- **Status**: todo\n\n"
                   "#### Pass 1.3 Third\n- **Status**: todo\n")});

    {   // The dialect signal sits AFTER 300 non-blank lines, so both sides must
        // miss it — and must miss it identically.
        QByteArray late = "# Demo — Roadmap\n\n";
        for (int i = 0; i < 400; ++i)
            late += "Plain prose line " + QByteArray::number(i) + ".\n";
        late += antsV1Roadmap(3);
        fixtures.push_back({"late-signal", late});
    }

    // Entirely blank AND larger than kDetectorByteCap. Below the cap it would
    // test only the line cap, and the byte cap is the newer of the two and the
    // one that could be applied to one producer and not the other.
    fixtures.push_back({"all-blank-2MiB", QByteArray(2 * 1024 * 1024, '\n')});

    int n = 0;
    for (const Fixture &fx : fixtures) {
        SCOPED_TRACE(fx.name);
        const QString path =
            dir.filePath(QStringLiteral("fx%1.md").arg(n++));
        ASSERT_TRUE(writeFile(path, fx.body));

        auto fileText = RoadmapText::fromFile(path);
        auto memText  = RoadmapText::fromMemory(QString::fromUtf8(readAllBytes(path)));

        const QStringList fromFilePrefix = fileText.detectionPrefix();
        const QStringList fromMemPrefix  = memText.detectionPrefix();

        EXPECT_EQ(fromFilePrefix, fromMemPrefix)
            << "the two producers cut at different places: file gave "
            << fromFilePrefix.size() << " lines, memory gave "
            << fromMemPrefix.size();

        bool sawFile = false, sawMem = false;
        const QString fmtFile =
            RoadmapParse::detectRoadmapFormat(fromFilePrefix, &sawFile);
        const QString fmtMem =
            RoadmapParse::detectRoadmapFormat(fromMemPrefix, &sawMem);
        EXPECT_EQ(fmtFile, fmtMem);
        EXPECT_EQ(sawFile, sawMem);

        // A fromMemory text reads no disk at all, so INV-1's bound holds
        // trivially there rather than being claimed of a path that cannot
        // meet it.
        EXPECT_EQ(memText.bytesRead(), 0);
    }
}

// --------------------------------------------------------- ANTS-3863 INV-4 --
// A migrated project whose live roadmap is ABSENT or unopenable still refuses,
// through the chain the header pins: empty prefix → sawSignal false →
// SourceUnrecognised. openFailed() must answer BEFORE any accessor has run,
// which is where every converted consumer branches on it.
TEST(RoadmapReadSeam, Ants3863Inv4UnopenableRoadmapStillRefuses) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = dir.filePath(QStringLiteral("proj"));
    const QString rm   = root + QStringLiteral("/ROADMAP.md");

    ASSERT_TRUE(writeFile(rm, antsV1Roadmap(4)));
    auto store = openStore(dir.filePath(QStringLiteral("store.sqlite")),
                           RoadmapStore::Access::Bulk);
    ASSERT_NE(store, nullptr);
    qint64 projectId = 0;
    ASSERT_TRUE(migrateInto(*store, root, &projectId));

    {   // Absent.
        auto text = RoadmapText::fromFile(root + QStringLiteral("/gone.md"));
        // Forced, not reported: this is the FIRST call on the object.
        EXPECT_TRUE(text.openFailed());
        EXPECT_TRUE(text.detectionPrefix().isEmpty());
        EXPECT_TRUE(text.full().isEmpty());

        auto fresh = RoadmapText::fromFile(root + QStringLiteral("/gone.md"));
        QString err;
        ReadError why = ReadError::None;
        const auto served =
            RoadmapSource::migratedProject(*store, root, fresh, &err, &why);
        EXPECT_FALSE(served.has_value());
        EXPECT_EQ(why, ReadError::SourceUnrecognised);
        EXPECT_TRUE(err.contains(QStringLiteral("unrecognisable")))
            << err.toStdString();
    }

    {   // Present and unreadable.
        ASSERT_TRUE(QFile::setPermissions(rm, QFile::Permissions()));
        auto text = RoadmapText::fromFile(rm);
        EXPECT_TRUE(text.openFailed());
        EXPECT_TRUE(text.detectionPrefix().isEmpty());
        QFile::setPermissions(rm, QFile::ReadOwner | QFile::WriteOwner);
    }

    // A fromMemory text has no file to fail on, so a site that branches on
    // openFailed() takes its success path unconditionally.
    auto mem = RoadmapText::fromMemory(QStringLiteral("anything"));
    EXPECT_FALSE(mem.openFailed());
}

// --------------------------------------------------------- ANTS-3863 INV-5 --
// full() is byte-identical to a QFile::readAll() of the same path opened
// ReadOnly | Text. The MODE is named in the assertion rather than assumed,
// because Text is exactly what decides the CRLF case — a comparison against a
// bare ReadOnly read would fail on a correct implementation.
//
// A NEW case, deliberately not an extension of Inv2BackendsAgree, which § 7
// requires to stay unedited but for its argument type.
TEST(RoadmapReadSeam, Ants3863Inv5FullMatchesReadAll) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const std::vector<std::pair<const char *, QByteArray>> cases = {
        {"trailing-newline",    antsV1Roadmap(5)},
        {"no-trailing-newline", QByteArray("# Demo\n\n## Work\n\nlast line, no newline")},
        {"crlf",                QByteArray("# Demo\r\n\r\n## Work\r\n\r\n- [ ] An item\r\n")},
        {"empty",               QByteArray()},
    };

    int n = 0;
    for (const auto &c : cases) {
        SCOPED_TRACE(c.first);
        const QString path = dir.filePath(QStringLiteral("inv5_%1.md").arg(n++));
        ASSERT_TRUE(writeFile(path, c.second));

        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString expected = QString::fromUtf8(f.readAll());
        f.close();

        // Cold: full() with no prefix read before it.
        auto cold = RoadmapText::fromFile(path);
        EXPECT_EQ(cold.full(), expected);

        // Warm: full() CONTINUING from where the prefix stopped. This is the
        // half that fails if the bounded reader's line splitting leaks into
        // full(), or if full() returns only the bytes after the prefix.
        auto warm = RoadmapText::fromFile(path);
        (void)warm.detectionPrefix();
        EXPECT_EQ(warm.full(), expected);
    }
}

// --------------------------------------------------------- ANTS-3863 INV-6 --
// No RoadmapText reads any byte of its file more than once, whichever order its
// accessors are called in. TWO legs, one per order, because § 2.1 satisfies the
// two by different mechanisms — continue the retained handle, versus derive the
// prefix from the memoised body — and a single-order test leaves one of them
// unexercised.
TEST(RoadmapReadSeam, Ants3863Inv6ReadsEveryByteAtMostOnce) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("inv6.md"));
    // Long enough that the prefix is a genuine proper prefix of the file.
    ASSERT_TRUE(writeFile(path, antsV1Roadmap(400)));
    const qint64 fileSize = QFileInfo(path).size();

    {   // Leg 1 — prefix, then body.
        SCOPED_TRACE("detectionPrefix() then full()");
        auto text = RoadmapText::fromFile(path);
        (void)text.detectionPrefix();
        const qint64 afterPrefix = text.bytesRead();
        EXPECT_GT(afterPrefix, 0);
        EXPECT_LT(afterPrefix, fileSize) << "the prefix drained the file";

        (void)text.full();
        EXPECT_EQ(text.bytesRead(), fileSize)
            << "file size plus the prefix means the head was read twice";

        // Both again: memoised, so nothing grows.
        (void)text.detectionPrefix();
        (void)text.full();
        EXPECT_EQ(text.bytesRead(), fileSize);
    }

    {   // Leg 2 — body, then prefix. The prefix is derived from the memoised
        // body through the same helper, so no disk is touched at all.
        SCOPED_TRACE("full() then detectionPrefix()");
        auto text = RoadmapText::fromFile(path);
        (void)text.full();
        EXPECT_EQ(text.bytesRead(), fileSize);

        (void)text.detectionPrefix();
        EXPECT_EQ(text.bytesRead(), fileSize)
            << "deriving the prefix from the body re-read the file";

        (void)text.full();
        (void)text.detectionPrefix();
        EXPECT_EQ(text.bytesRead(), fileSize);
    }

    // And the two orders agree on what the prefix IS.
    auto a = RoadmapText::fromFile(path);
    (void)a.detectionPrefix();
    const QStringList prefixFirst = a.detectionPrefix();
    auto b = RoadmapText::fromFile(path);
    (void)b.full();
    EXPECT_EQ(b.detectionPrefix(), prefixFirst);
}

// --------------------------------------------------------- ANTS-3863 INV-7 --
// No dispatch-taking entry point keeps a `const QString &markdown` overload.
//
// The matcher is MULTILINE on purpose: C++ parameter lists wrap, and seven of
// the twelve pre-change occurrences put `const QString &markdown,` on a line of
// its own — including migratedProject() and bulletsFor(), the two functions the
// whole item is about. A line-based grep saw only 5 of the 12, so reaching 0
// under one would have proved nothing about those two.
TEST(RoadmapReadSeam, Ants3863Inv7NoQStringMarkdownOverloadSurvives) {
    const QStringList files = {
        QStringLiteral("roadmapsource.h"),
        QStringLiteral("roadmapsource.cpp"),
        QStringLiteral("remotecontrol.h"),
        QStringLiteral("remotecontrol_internal.h"),
        QStringLiteral("remotecontrol_terminal.cpp"),
        QStringLiteral("remotecontrol_roadmap_query.cpp"),
        QStringLiteral("roadmapdialog.h"),
        QStringLiteral("roadmapdialog.cpp"),
    };
    QRegularExpression re(
        QStringLiteral("(roadmapBullets|roadmapWriteTarget|roadmapStoreServes"
                       "|bulletsFor|migratedProject)\\s*\\([^)]*QString\\s*&\\s*markdown"),
        QRegularExpression::DotMatchesEverythingOption);
    ASSERT_TRUE(re.isValid()) << re.errorString().toStdString();

    int total = 0;
    QStringList offenders;
    for (const QString &name : files) {
        const QString path =
            QStringLiteral(ANTS_SRC_DIR) + QStringLiteral("/") + name;
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::ReadOnly))
            << "cannot read " << path.toStdString()
            << " — the file list has drifted and this case is scanning nothing";
        const QString text = QString::fromUtf8(f.readAll());
        auto it = re.globalMatch(text);
        while (it.hasNext()) {
            it.next();
            ++total;
            offenders << name;
        }
    }
    EXPECT_EQ(total, 0)
        << "a dispatch-taking entry point still takes `const QString &markdown` "
           "in: " << offenders.join(QStringLiteral(", ")).toStdString()
        << ". A compatibility overload is the half-migrated seam — two ways to "
           "ask one question, free to disagree — that § 2.4's all-sites scope "
           "exists to prevent.";
}

// --------------------------------------------------------------- ANTS-4426 --
//
// The cost invariant has no behavioural witness, and that is the point: the two
// backends cannot disagree on a successful op:append (`roadmap_log_possible_
// duplicates.Ants4426FileAheadRefusesSoBackendsCannotDiffer` pins why), so
// re-introducing the file read would change no answer and no assertion. What it
// would change is 3.8 MiB of read-and-parse per append on this project.
//
// Comments are stripped before matching so the prose explaining the removal
// cannot satisfy the scrape it is explaining.
TEST(RoadmapReadSeam, Ants4426AppendAdvisoryReadsNoBody) {
    const QString path = QStringLiteral(ANTS_SRC_DIR)
                       + QStringLiteral("/remotecontrol_roadmap_log.cpp");
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly))
        << "cannot read " << path.toStdString()
        << " — the TU has moved and this case is scanning nothing";

    QStringList code;
    const QStringList lines =
        QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const int slashes = line.indexOf(QStringLiteral("//"));
        code << (slashes >= 0 ? line.left(slashes) : line);
    }
    const QString stripped = code.join(QLatin1Char('\n'));

    EXPECT_FALSE(stripped.contains(QStringLiteral("storeText.full()")))
        << "roadmap_log's store path reads the whole of ROADMAP.md again. The "
           "near-duplicate advisory was its last consumer and now takes its "
           "records from the store (ANTS-4426); a body read here spends the "
           "whole of ANTS-3863's saving on this verb.";
}

// --------------------------------------------------------------- ANTS-4431 --
//
// A cold `roadmap_query section=` used to read ROADMAP.md TWICE. The miss
// block's provider built the heading index from full(); the section branch then
// constructed a SECOND file-backed RoadmapText — in the same function, so it
// could not reach the first's memo — and called full() again to slice the
// section for the ANTS-1907 etag. 7.6 MiB of read and two 3.8 MiB QStrings per
// cold section query on this project.
//
// Like ANTS-4426's case above, the cost invariant has no behavioural witness,
// and that is the point: both providers read the same path at the same mtime,
// so they could never disagree. What a second provider costs is the read. So
// the invariant is that cmdRoadmapQuery constructs exactly ONE, and every
// branch shares it.
//
// The slice is anchored on the function's own definition line and the next
// top-level definition, NOT on a byte window — a comment added above the
// function must not slide the window off the body.
TEST(RoadmapReadSeam, Ants4431RoadmapQueryConstructsOneProvider) {
    const QString path = QStringLiteral(ANTS_SRC_DIR)
                       + QStringLiteral("/remotecontrol_roadmap_query.cpp");
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly))
        << "cannot read " << path.toStdString()
        << " — the TU has moved and this case is scanning nothing";

    const QStringList lines =
        QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));

    const QString opener =
        QStringLiteral("QJsonDocument RemoteControl::cmdRoadmapQuery(");
    const QString anyDefn = QStringLiteral("QJsonDocument RemoteControl::");
    int begin = -1;
    int end   = -1;
    for (int i = 0; i < lines.size(); ++i) {
        if (begin < 0) {
            if (lines.at(i).startsWith(opener))
                begin = i;
            continue;
        }
        if (lines.at(i).startsWith(anyDefn)) {
            end = i;
            break;
        }
    }
    ASSERT_GE(begin, 0)
        << "cmdRoadmapQuery's definition line has changed shape — this case is "
           "scanning nothing";
    ASSERT_GT(end, begin)
        << "no top-level definition follows cmdRoadmapQuery — the slice ran to "
           "EOF and is not the function";

    // Comments are stripped before counting so the prose explaining the hoist
    // cannot satisfy the scrape it is explaining.
    int providers = 0;
    for (int i = begin; i < end; ++i) {
        const QString &line = lines.at(i);
        const int slashes   = line.indexOf(QStringLiteral("//"));
        const QString code  = (slashes >= 0 ? line.left(slashes) : line);
        providers += static_cast<int>(
            code.count(QStringLiteral("RoadmapText::fromFile")));
    }

    EXPECT_EQ(providers, 1)
        << "cmdRoadmapQuery constructs " << providers << " RoadmapText "
           "providers. Each opens and reads ROADMAP.md on its own, and one "
           "cannot reuse another's memo, so a second is a second whole-file "
           "read in one call (ANTS-4431). Hoist it above `if (!fresh)` and let "
           "every branch share it.";
}
