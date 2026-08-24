// Feature-conformance test for ANTS-4070 — rotate a closed minor into its
// archive, and retitle a section. Contract:
// tests/features/roadmap_rotate_minor/spec.md
// Design: docs/specs/ANTS-4070-rotation-and-section-title.md
//
// Behavioural, through the roadmap_log verbs themselves: every case migrates a
// markdown fixture into a store at RoadmapStore::defaultPath() (redirected into
// the case's sandbox), drives a `*ForTest` entry point, and re-opens the store
// to assert what landed. Migrating rather than hand-building matters — a
// hand-built store can hold rows the loader never writes, and an invariant
// asserted against one is asserted against a state the product cannot reach.

#include "../../_support/expect.h"
#include "../../_support/xdg_guard.h"

#include "remotecontrol.h"
#include "roadmapindex.h"
#include "roadmapmigrate.h"
#include "roadmapmigrateload.h"
#include "roadmapstore.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>
#include <QVector>

#include <algorithm>
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
// REAL store under XDG_DATA_HOME. Every case redirects XDG_DATA_HOME first, and
// `Access` is the THIRD parameter, after the history cap.
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

// ~1.2 KiB of intro prose, so the file clears kRoadmapMinParseableSize (1024 B)
// — below it the write paths will not trust an ants-v1 walk and refuse
// unrecognised_format before any op is tried.
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

QByteArray header() {
    QByteArray b =
        "<!-- ants-roadmap-format: 1 -->\n"
        "\n"
        "# Demo \xE2\x80\x94 Roadmap\n"
        "\n";
    b += kPad;
    b += "\n";
    return b;
}

// One bullet. `emoji` is the raw UTF-8 status marker; `layman` false omits the
// Layman: line, which is the render's publish-gate offence (INV-10 only).
QByteArray bullet(const char *emoji, const char *id, const char *headline,
                  bool layman = true) {
    QByteArray b = QByteArray("- ") + emoji + " [" + id + "] **" + headline + "**\n";
    if (layman) b += "  Layman: A thing.\n";
    b += "  Kind: fix.\n"
         "  Source: seed.\n"
         "\n";
    return b;
}

const char *kShipped = "\xE2\x9C\x85";      // ✅
const char *kPlanned = "\xF0\x9F\x93\x8B";  // 📋
const char *kProgress = "\xF0\x9F\x9A\xA7"; // 🚧
const char *kConsidered = "\xF0\x9F\x92\xAD"; // 💭

const char *kClosedTitle = "0.7.0 \xE2\x80\x94 first release \xE2\x80\x94 shipped 2026-01-01";
const char *kChildTitle  = "0.7.0 details";
const char *kOpenTitle   = "0.8.0 \xE2\x80\x94 in flight";

// The shared two-minor fixture: a closed 0.7 with a `##`, a `###` child and a
// bullet in each, and an open 0.8. `openEmoji` lets INV-3 put an open bullet
// inside the CLOSED minor; `childEmoji` puts one under its child.
QByteArray twoMinorFixture(const char *closedEmoji = nullptr,
                           const char *childEmoji  = nullptr,
                           bool openItemHasLayman  = true) {
    QByteArray b = header();
    b += QByteArray("## ") + kClosedTitle + "\n\n";
    b += bullet(kShipped, "DEMO-0001", "A shipped item.");
    if (closedEmoji) b += bullet(closedEmoji, "DEMO-0004", "An unfinished item.");
    b += QByteArray("### ") + kChildTitle + "\n\n";
    b += bullet(kShipped, "DEMO-0002", "A shipped child item.");
    if (childEmoji) b += bullet(childEmoji, "DEMO-0005", "An unfinished child item.");
    b += QByteArray("## ") + kOpenTitle + "\n\n";
    b += bullet(kPlanned, "DEMO-0003", "An open item.", openItemHasLayman);
    return b;
}

// Redirect XDG_DATA_HOME into the sandbox, write the fixture, and migrate it.
// Bulk, because RoadmapMigrateLoad::load() refuses an Interactive connection
// (ANTS-3765 INV-12); closed before the verb under test runs.
QString seedMigrated(ants_test::XdgGuard &guard, const QTemporaryDir &tmp,
                     const QByteArray &markdown, qint64 *projectId) {
    guard.setEnv("XDG_DATA_HOME",
                 QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());

    const QString rawRoot = QDir(tmp.path()).filePath(QStringLiteral("proj"));
    if (!writeFile(rawRoot + QStringLiteral("/ROADMAP.md"), markdown))
        return QString();
    // The store keys a project on its CANONICAL root (ANTS-3756 INV-8), and
    // /tmp is a symlink on some hosts.
    const QString root = QFileInfo(rawRoot).canonicalFilePath();

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
    opts.changedAt   = QStringLiteral("2026-08-10T10:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(*store, plan, opts);
    if (!out.ok) {
        ADD_FAILURE() << "migration load: " << out.error.toStdString();
        return QString();
    }
    *projectId = out.projectId;
    return root;
}

// Re-run the migration over whatever the render just published. INV-11 and
// INV-13 both claim the store equals its own regeneration, and only a real
// round trip can show that: a test asserting the expected slug directly passes
// against an implementation whose slug rule is self-consistent and disagrees
// with the importer's.
bool reimport(const QString &root) {
    auto store = openStore(RoadmapStore::Access::Bulk);
    if (!store) return false;
    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    if (!disc) {
        ADD_FAILURE() << "re-import findRoadmaps: " << err.toStdString();
        return false;
    }
    const auto plan =
        RoadmapMigrate::planFrom(*disc, QStringLiteral("Demo"), QStringLiteral("demo"));
    RoadmapMigrateLoad::Options opts;
    opts.changedAt   = QStringLiteral("2026-08-10T11:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(*store, plan, opts);
    if (!out.ok) {
        ADD_FAILURE() << "re-import load: " << out.error.toStdString();
        return false;
    }
    return true;
}

QVector<RoadmapStore::SectionRow> sectionsOf(qint64 projectId) {
    auto store = openStore(RoadmapStore::Access::Interactive);
    if (!store) return {};
    QString err;
    const auto rows = store->listSections(projectId, &err);
    if (!rows) {
        ADD_FAILURE() << "listSections: " << err.toStdString();
        return {};
    }
    return *rows;
}

// The stored slug of the `nth` (0-based) section whose title matches exactly.
// Titles rather than slugs, because INV-12's fixture deliberately holds two
// sections with the same title and therefore two DIFFERENT slugs.
QString slugOfTitle(qint64 projectId, const QString &title, int nth = 0) {
    int seen = 0;
    for (const auto &s : sectionsOf(projectId)) {
        if (s.title != title) continue;
        if (seen++ == nth) return s.slug;
    }
    return QString();
}

std::optional<RoadmapStore::SectionRow> rowOfSlug(qint64 projectId,
                                                  const QString &slug) {
    for (const auto &s : sectionsOf(projectId))
        if (s.slug == slug) return s;
    return std::nullopt;
}

std::optional<qint64> idOfSlug(qint64 projectId, const QString &slug) {
    auto store = openStore(RoadmapStore::Access::Interactive);
    if (!store) return std::nullopt;
    QString err;
    return store->findSection(projectId, slug, &err);
}

// One bullet's RENDERED text: the `- ` line carrying `id` plus its
// continuation lines, trailing blanks trimmed.
QString bulletBlock(const QByteArray &file, const QString &id) {
    const QStringList lines = QString::fromUtf8(file).split(QChar('\n'));
    int start = -1;
    for (int i = 0; i < lines.size(); ++i) {
        if (lines.at(i).startsWith(QLatin1String("- ")) && lines.at(i).contains(id)) {
            start = i;
            break;
        }
    }
    if (start < 0) return QString();
    int end = start + 1;
    while (end < lines.size() && !lines.at(end).startsWith(QLatin1String("- "))
           && !lines.at(end).startsWith(QChar('#')))
        ++end;
    QStringList block = lines.mid(start, end - start);
    while (!block.isEmpty() && block.last().trimmed().isEmpty())
        block.removeLast();
    return block.join(QChar('\n'));
}

// Diagnostic for the two invariants that compare SETS of sections — a bare
// count mismatch says nothing about which section appeared or moved.
std::string dumpSections(qint64 projectId) {
    QStringList out;
    for (const auto &s : sectionsOf(projectId))
        out << QStringLiteral("  [L%1] %2  src=%3  title=%4")
                   .arg(s.level)
                   .arg(s.slug, s.sourcePath.value_or(QStringLiteral("<live>")),
                        s.title);
    return ('\n' + out.join(QChar('\n'))).toStdString();
}

QJsonObject rotateReq(const QString &root, const QString &minor,
                      bool dryRun = false) {
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("op")]         = QStringLiteral("rotate_minor");
    req[QStringLiteral("minor")]      = minor;
    if (dryRun) req[QStringLiteral("dry_run")] = true;
    return req;
}

QJsonObject retitleReq(const QString &root, const QString &slug,
                       const QString &title, bool dryRun = false) {
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("op")]         = QStringLiteral("retitle_section");
    req[QStringLiteral("section")]    = slug;
    req[QStringLiteral("title")]      = title;
    if (dryRun) req[QStringLiteral("dry_run")] = true;
    return req;
}

QJsonObject rotate(const QJsonObject &req) {
    RemoteControl rc(nullptr);
    return rc.cmdRoadmapLogRotateMinorForTest(req).object();
}

QJsonObject retitle(const QJsonObject &req) {
    RemoteControl rc(nullptr);
    return rc.cmdRoadmapLogRetitleSectionForTest(req).object();
}

// A render that settles the fixture. The FIRST render of hand-written markdown
// legitimately materialises trailers the source omitted (the settling ANTS-4065
// § 2.6 measures), so INV-1's byte-identical comparison is only meaningful
// against already-rendered text. Appending to the OPEN minor leaves the closed
// one untouched.
void settleRender(const QString &root) {
    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("op")]         = QStringLiteral("append");
    req[QStringLiteral("section")]    =
        RoadmapIndex::slugifyHeading(QString::fromUtf8(kOpenTitle));
    req[QStringLiteral("status")]   = QStringLiteral("planned");
    req[QStringLiteral("headline")] = QStringLiteral("A settling item.");
    req[QStringLiteral("kind")]     = QStringLiteral("implement");
    req[QStringLiteral("source")]   = QStringLiteral("test");
    req[QStringLiteral("layman")]   = QStringLiteral("Renders the fixture once.");
    const QJsonObject resp = rc.cmdRoadmapLogAppendForTest(req).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << "settling render: "
        << resp.value(QStringLiteral("code")).toString().toStdString() << ": "
        << resp.value(QStringLiteral("error")).toString().toStdString();
}

QString livePath(const QString &root)    { return root + QStringLiteral("/ROADMAP.md"); }
QString archivePath(const QString &root) { return root + QStringLiteral("/docs/roadmap/0.7.md"); }

}  // namespace

// ---------------------------------------------------------------- INV-1 -----

// A rotated minor's sections render into docs/roadmap/<M>.<N>.md and out of
// ROADMAP.md, with their content unchanged.
TEST(RoadmapRotateMinor, Inv1SectionsMoveWithContentUnchanged) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, twoMinorFixture(), &projectId);
    ASSERT_FALSE(root.isEmpty());
    settleRender(root);

    const QByteArray liveBefore = readAll(livePath(root));
    const QString b1 = bulletBlock(liveBefore, QStringLiteral("DEMO-0001"));
    const QString b2 = bulletBlock(liveBefore, QStringLiteral("DEMO-0002"));
    ASSERT_FALSE(b1.isEmpty());
    ASSERT_FALSE(b2.isEmpty());

    const QJsonObject resp = rotate(rotateReq(root, QStringLiteral("0.7")));
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("code")).toString().toStdString() << ": "
        << resp.value(QStringLiteral("error")).toString().toStdString();

    const QByteArray liveAfter = readAll(livePath(root));
    const QByteArray archive   = readAll(archivePath(root));
    ASSERT_FALSE(archive.isEmpty()) << "the archive must have been rendered";

    EXPECT_FALSE(liveAfter.contains("DEMO-0001"));
    EXPECT_FALSE(liveAfter.contains("DEMO-0002"));
    EXPECT_TRUE(liveAfter.contains("DEMO-0003"))
        << "the open minor must stay in the live file";
    EXPECT_TRUE(archive.contains("DEMO-0001"));
    EXPECT_TRUE(archive.contains("DEMO-0002"));

    EXPECT_EQ(bulletBlock(archive, QStringLiteral("DEMO-0001")), b1)
        << "rotation moves a bullet; it must not rewrite it";
    EXPECT_EQ(bulletBlock(archive, QStringLiteral("DEMO-0002")), b2);
}

// ---------------------------------------------------------------- INV-2 -----

// Rotation is idempotent and the second run SUCCEEDS — the emptiness check is
// keyed on the title match, not on the set that survives the already-archived
// skip. A minor no title matches is the case that really does refuse.
TEST(RoadmapRotateMinor, Inv2IdempotentAndDistinctFromNoMatch) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, twoMinorFixture(), &projectId);
    ASSERT_FALSE(root.isEmpty());

    const QJsonObject first = rotate(rotateReq(root, QStringLiteral("0.7")));
    ASSERT_TRUE(first.value(QStringLiteral("ok")).toBool())
        << first.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_GT(first.value(QStringLiteral("sections_moved")).toInt(), 0);

    const QByteArray liveAfterFirst    = readAll(livePath(root));
    const QByteArray archiveAfterFirst = readAll(archivePath(root));

    const QJsonObject second = rotate(rotateReq(root, QStringLiteral("0.7")));
    ASSERT_TRUE(second.value(QStringLiteral("ok")).toBool())
        << "a re-run must succeed, not refuse: "
        << second.value(QStringLiteral("code")).toString().toStdString();
    EXPECT_EQ(second.value(QStringLiteral("sections_moved")).toInt(), 0);
    EXPECT_TRUE(second.value(QStringLiteral("sections")).toArray().isEmpty());

    EXPECT_EQ(readAll(livePath(root)), liveAfterFirst);
    EXPECT_EQ(readAll(archivePath(root)), archiveAfterFirst);

    // The other empty case, so the two are shown to be distinguishable rather
    // than assumed to be.
    const QJsonObject nomatch = rotate(rotateReq(root, QStringLiteral("0.9")));
    EXPECT_FALSE(nomatch.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(nomatch.value(QStringLiteral("code")).toString(),
              QStringLiteral("section_not_found"));
}

// ---------------------------------------------------------------- INV-3 -----

// A minor holding an open item is refused, and nothing is written. 💭 counts as
// open — RoadmapRender::isOpen() says so — and the guard covers the whole MOVE
// SET, so an open item under a `###` child refuses too.
TEST(RoadmapRotateMinor, Inv3OpenItemRefuses) {
    struct Case {
        const char *what;
        const char *closedEmoji;
        const char *childEmoji;
    } cases[] = {
        { "planned in the ## section",    kPlanned,    nullptr },
        { "in-progress in the ## section", kProgress,  nullptr },
        { "considered in the ## section", kConsidered, nullptr },
        { "in-progress under the ### child", nullptr,  kProgress },
    };

    for (const Case &c : cases) {
        SCOPED_TRACE(c.what);
        ants_test::XdgGuard guard;
        QTemporaryDir tmp;
        ASSERT_TRUE(tmp.isValid());
        qint64 projectId = 0;
        const QString root = seedMigrated(
            guard, tmp, twoMinorFixture(c.closedEmoji, c.childEmoji), &projectId);
        ASSERT_FALSE(root.isEmpty());

        const QByteArray before = readAll(livePath(root));
        ASSERT_FALSE(before.isEmpty());

        const QJsonObject resp = rotate(rotateReq(root, QStringLiteral("0.7")));
        EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
        EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
                  QStringLiteral("minor_not_closed"));
        EXPECT_EQ(readAll(livePath(root)), before);
        EXPECT_FALSE(QFile::exists(archivePath(root)))
            << "a refused rotation must not create the archive";
    }
}

// ---------------------------------------------------------------- INV-4 -----

// The match admits only a release designator: `0.7` claims `## 0.7.0` and
// `## 0.7` but not `## 0.70.0`, and `0.5` does not claim the two-minor signpost.
TEST(RoadmapRotateMinor, Inv4MatchAdmitsOnlyAReleaseDesignator) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const char *kBare     = "0.7";
    const char *kDecoy    = "0.70.0 \xE2\x80\x94 a much later release";
    const char *kSignpost = "0.5.x and 0.6.x \xE2\x80\x94 archived";

    QByteArray md = header();
    md += QByteArray("## ") + kClosedTitle + "\n\n";
    md += bullet(kShipped, "DEMO-0001", "A shipped item.");
    md += QByteArray("## ") + kBare + "\n\n";
    md += bullet(kShipped, "DEMO-0002", "A bare-minor item.");
    md += QByteArray("## ") + kDecoy + "\n\n";
    md += bullet(kShipped, "DEMO-0003", "A decoy item.");
    md += QByteArray("## ") + kSignpost + "\n\n";
    md += bullet(kShipped, "DEMO-0004", "A signpost item.");

    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, md, &projectId);
    ASSERT_FALSE(root.isEmpty());

    const QString decoySlug =
        RoadmapIndex::slugifyHeading(QString::fromUtf8(kDecoy));
    const QString signpostSlug =
        RoadmapIndex::slugifyHeading(QString::fromUtf8(kSignpost));

    const QJsonObject resp = rotate(rotateReq(root, QStringLiteral("0.7")));
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("sections_moved")).toInt(), 2)
        << "exactly `## 0.7.0 …` and `## 0.7` move";

    const QByteArray archive = readAll(archivePath(root));
    EXPECT_TRUE(archive.contains("DEMO-0001"));
    EXPECT_TRUE(archive.contains("DEMO-0002"));
    EXPECT_FALSE(archive.contains("DEMO-0003"))
        << "`## 0.70.0` is not in minor 0.7 — a plain startsWith claims it";
    EXPECT_FALSE(archive.contains("DEMO-0004"));

    // The decoy and the signpost are still in the live file, unmoved.
    const auto decoy = rowOfSlug(projectId, decoySlug);
    ASSERT_TRUE(decoy.has_value());
    EXPECT_FALSE(decoy->sourcePath.has_value());
    const auto signpost = rowOfSlug(projectId, signpostSlug);
    ASSERT_TRUE(signpost.has_value());
    EXPECT_FALSE(signpost->sourcePath.has_value());

    // `.x` is a `.` followed by a NON-digit, so the third case excludes the
    // signpost without a special case — and nothing else matches 0.5.
    const QJsonObject fivish = rotate(rotateReq(root, QStringLiteral("0.5")));
    EXPECT_FALSE(fivish.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(fivish.value(QStringLiteral("code")).toString(),
              QStringLiteral("section_not_found"))
        << "moving the signpost into the archive it points at deletes the "
           "pointer and duplicates its text inside its own target";
}

// ---------------------------------------------------------------- INV-5 -----

// Every descendant moves with its parent, including one misfiled under a
// DIFFERENT archive — the skip is on equality with the derived path, not on
// "already archived".
TEST(RoadmapRotateMinor, Inv5DescendantsMoveIncludingAMisfiledOne) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const char *kSecondChild = "0.7.0 addenda";

    QByteArray md = header();
    md += QByteArray("## ") + kClosedTitle + "\n\n";
    md += bullet(kShipped, "DEMO-0001", "A shipped item.");
    md += QByteArray("### ") + kChildTitle + "\n\n";
    md += bullet(kShipped, "DEMO-0002", "A shipped child item.");
    md += QByteArray("### ") + kSecondChild + "\n\n";
    md += bullet(kShipped, "DEMO-0006", "A misfiled child item.");
    md += QByteArray("## ") + kOpenTitle + "\n\n";
    md += bullet(kPlanned, "DEMO-0003", "An open item.");

    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, md, &projectId);
    ASSERT_FALSE(root.isEmpty());

    const QString parentSlug =
        RoadmapIndex::slugifyHeading(QString::fromUtf8(kClosedTitle));
    const QString childSlug =
        RoadmapIndex::slugifyHeading(QString::fromUtf8(kChildTitle));
    const QString misfiledSlug =
        RoadmapIndex::slugifyHeading(QString::fromUtf8(kSecondChild));

    // Misfile the second child directly. The loader cannot produce this state
    // from markdown — a `###` inside 0.6.md would have its parent inside 0.6.md
    // too — and it is exactly the state a skip phrased over "any non-null
    // sourcePath" would strand.
    {
        auto store = openStore(RoadmapStore::Access::Interactive);
        ASSERT_NE(store, nullptr);
        QString err;
        const auto sid = store->findSection(projectId, misfiledSlug, &err);
        ASSERT_TRUE(sid.has_value()) << err.toStdString();
        ASSERT_TRUE(store->setSectionSource(
            *sid, QStringLiteral("docs/roadmap/0.6.md"), &err))
            << err.toStdString();
    }

    const QJsonObject resp = rotate(rotateReq(root, QStringLiteral("0.7")));
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("code")).toString().toStdString() << ": "
        << resp.value(QStringLiteral("error")).toString().toStdString();

    const QString derived = QStringLiteral("docs/roadmap/0.7.md");
    for (const QString &slug : { parentSlug, childSlug, misfiledSlug }) {
        SCOPED_TRACE(slug.toStdString());
        // The slug moved with the section, so look the row up by its stored id.
        bool found = false;
        for (const auto &s : sectionsOf(projectId)) {
            if (!s.slug.endsWith(slug)) continue;
            found = true;
            ASSERT_TRUE(s.sourcePath.has_value());
            EXPECT_EQ(*s.sourcePath, derived);
        }
        EXPECT_TRUE(found) << "the section vanished from the store";
    }
}

// ---------------------------------------------------------------- INV-6 -----

// The archive path is DERIVED, project-root-relative and § 3.9-conforming; and
// a rotation never empties the live file.
TEST(RoadmapRotateMinor, Inv6DerivedPathAndNeverEmptyLive) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, twoMinorFixture(), &projectId);
    ASSERT_FALSE(root.isEmpty());

    for (const char *bad : { "v0.7", "0.7.0", "00.7" }) {
        SCOPED_TRACE(bad);
        const QJsonObject resp =
            rotate(rotateReq(root, QString::fromLatin1(bad)));
        EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
        EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
                  QStringLiteral("bad_args"));
    }

    const QJsonObject ok =
        rotate(rotateReq(root, QStringLiteral("0.7"), /*dryRun=*/true));
    ASSERT_TRUE(ok.value(QStringLiteral("ok")).toBool())
        << ok.value(QStringLiteral("error")).toString().toStdString();
    const QString derived = ok.value(QStringLiteral("archive_path")).toString();
    EXPECT_EQ(derived, QStringLiteral("docs/roadmap/0.7.md"));
    // The same predicate RoadmapMigrateLoad::isPlaceableSourcePath() applies,
    // so this is asserted against the rule that actually gates a re-import
    // rather than against a restatement of it.
    static const QRegularExpression placeable(QStringLiteral(
        "\\Adocs/roadmap/(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.md\\z"));
    EXPECT_TRUE(placeable.match(derived).hasMatch()) << derived.toStdString();
}

TEST(RoadmapRotateMinor, Inv6NeverEmptiesTheLiveFile) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    // Every section belongs to the rotated minor, so the move would leave the
    // live file with none — and the render never rewrites a file with no
    // sections, stranding its old content on disk.
    //
    // The fixture starts on its `##` line with NO preamble, and that is the
    // whole point: content before the first heading becomes a level-0 preamble
    // section which stays on the live path, so a fixture carrying one can never
    // reach zero live sections and would test nothing. The pad is the SECTION's
    // intro instead of the file's.
    QByteArray md = QByteArray("## ") + kClosedTitle + "\n\n";
    md += kPad;
    md += "\n";
    md += bullet(kShipped, "DEMO-0001", "A shipped item.");
    md += QByteArray("### ") + kChildTitle + "\n\n";
    md += bullet(kShipped, "DEMO-0002", "A shipped child item.");

    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, md, &projectId);
    ASSERT_FALSE(root.isEmpty());

    const QByteArray before = readAll(livePath(root));
    const QJsonObject resp = rotate(rotateReq(root, QStringLiteral("0.7")));
    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool())
        << dumpSections(projectId);
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_args"));
    EXPECT_EQ(readAll(livePath(root)), before);
    EXPECT_FALSE(QFile::exists(archivePath(root)));
}

// ---------------------------------------------------------------- INV-7 -----

// A retitle changes the title and the slug, and nothing else.
TEST(RoadmapRotateMinor, Inv7RetitleChangesTitleAndSlugOnly) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, twoMinorFixture(), &projectId);
    ASSERT_FALSE(root.isEmpty());

    const QString oldSlug =
        RoadmapIndex::slugifyHeading(QString::fromUtf8(kClosedTitle));
    const auto before = rowOfSlug(projectId, oldSlug);
    ASSERT_TRUE(before.has_value());
    const auto beforeId = idOfSlug(projectId, oldSlug);
    ASSERT_TRUE(beforeId.has_value());

    const QString newTitle =
        QString::fromUtf8("0.7.0 \xE2\x80\x94 first release \xE2\x80\x94 shipped (2026-01-02)");
    const QString expectedSlug = RoadmapIndex::slugifyHeading(newTitle);

    const QJsonObject resp = retitle(retitleReq(root, oldSlug, newTitle));
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("code")).toString().toStdString() << ": "
        << resp.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("slug")).toString(), expectedSlug);
    EXPECT_EQ(resp.value(QStringLiteral("previous_slug")).toString(), oldSlug);

    const auto after = rowOfSlug(projectId, expectedSlug);
    ASSERT_TRUE(after.has_value()) << "the section must be addressable by its "
                                     "new slug";
    EXPECT_EQ(after->title, newTitle);
    EXPECT_EQ(after->level, before->level);
    EXPECT_EQ(after->position, before->position);
    EXPECT_EQ(after->parentId, before->parentId);
    EXPECT_EQ(after->sourcePath, before->sourcePath);
    EXPECT_EQ(idOfSlug(projectId, expectedSlug), beforeId)
        << "a retitle re-addresses a section; it does not replace it";

    // The items filed under it are untouched, and the rendered heading carries
    // the new text.
    const QByteArray live = readAll(livePath(root));
    EXPECT_TRUE(live.contains(newTitle.toUtf8()));
    EXPECT_TRUE(live.contains("DEMO-0001"));
    EXPECT_TRUE(live.contains("DEMO-0002"));
}

// ---------------------------------------------------------------- INV-8 -----

// A dry run writes nothing and reports what a real run would do — for BOTH
// operations. The retitle leg is the one that catches a dry run built to return
// only what it was asked, since the new slug is computed rather than passed.
TEST(RoadmapRotateMinor, Inv8DryRunMatchesTheRealRun) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, twoMinorFixture(), &projectId);
    ASSERT_FALSE(root.isEmpty());

    // Leg 1 — rotate_minor.
    const QByteArray liveBefore = readAll(livePath(root));
    const QJsonObject dry =
        rotate(rotateReq(root, QStringLiteral("0.7"), /*dryRun=*/true));
    ASSERT_TRUE(dry.value(QStringLiteral("ok")).toBool())
        << dry.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_EQ(readAll(livePath(root)), liveBefore);
    EXPECT_FALSE(QFile::exists(archivePath(root)));

    const QJsonObject real = rotate(rotateReq(root, QStringLiteral("0.7")));
    ASSERT_TRUE(real.value(QStringLiteral("ok")).toBool())
        << real.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_EQ(dry.value(QStringLiteral("archive_path")),
              real.value(QStringLiteral("archive_path")));
    EXPECT_EQ(dry.value(QStringLiteral("sections_moved")),
              real.value(QStringLiteral("sections_moved")));
    EXPECT_EQ(dry.value(QStringLiteral("sections")).toArray(),
              real.value(QStringLiteral("sections")).toArray());
    EXPECT_EQ(dry.value(QStringLiteral("sections")).toArray().size(),
              dry.value(QStringLiteral("sections_moved")).toInt())
        << "sections_moved is always sections.length";

    // Leg 2 — retitle_section, on the open minor (the closed one has moved).
    const QString slug =
        RoadmapIndex::slugifyHeading(QString::fromUtf8(kOpenTitle));
    const QString newTitle = QStringLiteral("0.8.0 — shipped (2026-02-01)");
    const QByteArray live2 = readAll(livePath(root));
    const QByteArray arch2 = readAll(archivePath(root));

    const QJsonObject dryR =
        retitle(retitleReq(root, slug, newTitle, /*dryRun=*/true));
    ASSERT_TRUE(dryR.value(QStringLiteral("ok")).toBool())
        << dryR.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_EQ(readAll(livePath(root)), live2);
    EXPECT_EQ(readAll(archivePath(root)), arch2);

    const QJsonObject realR = retitle(retitleReq(root, slug, newTitle));
    ASSERT_TRUE(realR.value(QStringLiteral("ok")).toBool())
        << realR.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_EQ(dryR.value(QStringLiteral("slug")), realR.value(QStringLiteral("slug")));
    EXPECT_EQ(dryR.value(QStringLiteral("previous_slug")),
              realR.value(QStringLiteral("previous_slug")));
    EXPECT_EQ(dryR.value(QStringLiteral("title")), realR.value(QStringLiteral("title")));
    EXPECT_FALSE(dryR.value(QStringLiteral("slug")).toString().isEmpty())
        << "the new slug is COMPUTED, and the preview is where a caller sees "
           "the address its section is about to move to";
}

// ---------------------------------------------------------------- INV-9 -----

// retitle_section's arguments are validated, and each failure has one code a
// caller can branch on.
TEST(RoadmapRotateMinor, Inv9RetitleArgumentValidation) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, twoMinorFixture(), &projectId);
    ASSERT_FALSE(root.isEmpty());

    const QString slug =
        RoadmapIndex::slugifyHeading(QString::fromUtf8(kClosedTitle));
    const QString original = QString::fromUtf8(kClosedTitle);

    struct BadTitle { const char *what; QString title; };
    const QVector<BadTitle> badTitles = {
        { "empty",             QString() },
        { "whitespace only",   QStringLiteral("   ") },
        { "contains newline",  QStringLiteral("Alpha\nBeta") },
        // Neither empty nor whitespace-only, and it slugifies to "" —
        // slugifyHeading() keeps only letters and digits, and uniqueSlug()
        // returns an empty base WITHOUT inserting it into `seen`, so it never
        // disambiguates one empty slug from another.
        { "punctuation only",  QString::fromUtf8("\xE2\x80\x94\xE2\x80\x94\xE2\x80\x94") },
    };
    for (const BadTitle &bt : badTitles) {
        SCOPED_TRACE(bt.what);
        const QJsonObject resp = retitle(retitleReq(root, slug, bt.title));
        EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
        EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
                  QStringLiteral("bad_args"));
        const auto row = rowOfSlug(projectId, slug);
        ASSERT_TRUE(row.has_value());
        EXPECT_EQ(row->title, original);
    }

    const QJsonObject unresolvable =
        retitle(retitleReq(root, QStringLiteral("no-such-section"),
                           QStringLiteral("Anything")));
    EXPECT_FALSE(unresolvable.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(unresolvable.value(QStringLiteral("code")).toString(),
              QStringLiteral("section_not_found"));

    {
        QJsonObject req = retitleReq(root, slug, QStringLiteral("Anything"));
        req.remove(QStringLiteral("section"));
        const QJsonObject resp = retitle(req);
        EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
        EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
                  QStringLiteral("missing_field"));
    }
    {
        QJsonObject req = retitleReq(root, slug, QStringLiteral("Anything"));
        req.remove(QStringLiteral("title"));
        const QJsonObject resp = retitle(req);
        EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
        EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
                  QStringLiteral("missing_field"));
    }

    const auto row = rowOfSlug(projectId, slug);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->title, original);
}

// --------------------------------------------------------------- INV-10 -----

// The publish gate is inherited, not bypassed — but since ANTS-4628 what is
// inherited is a SCOPED gate, and a rotation touches no item row (it moves
// `section` and `element` rows), so its scope is engaged-empty and no item
// refuses it. An offender under the OPEN minor no longer blocks a rotation of
// the closed one, which is what this case asserted until 2026-08-24.
//
// This is not the gate being skipped. The rotation runs the same
// `commitAndRender` sequence as every other write; the gate runs and judges the
// empty set the rotation touched. Worth noting how narrow the old behaviour
// always was: `minor_not_closed` already refuses a rotation whose move set
// holds an open item, and closed items are never gated — so the only thing this
// could ever fire on was debt somewhere else in the project, which is exactly
// what the amendment stopped holding against unrelated writes.
TEST(RoadmapRotateMinor, Inv10PublishGateScopedNotBypassed) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(
        guard, tmp,
        twoMinorFixture(nullptr, nullptr, /*openItemHasLayman=*/false),
        &projectId);
    ASSERT_FALSE(root.isEmpty());

    const QByteArray before = readAll(livePath(root));
    const QJsonObject resp = rotate(rotateReq(root, QStringLiteral("0.7")));
    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << "an offender under a different minor must no longer refuse a "
           "rotation; got code "
        << resp.value(QStringLiteral("code")).toString().toStdString();

    // The rotation actually happened — the archive exists and the live file
    // changed. Without these the case would pass on a no-op that never ran.
    EXPECT_TRUE(QFile::exists(archivePath(root)))
        << "the rotation must have published its archive";
    EXPECT_NE(readAll(livePath(root)), before)
        << "the rotation must have rewritten the live roadmap";

    // That an untouched offender is not cured behind the caller's back is
    // asserted in tests/features/roadmap_write_half
    // (Ants4628UntouchedDebtDoesNotBlockAWrite), which has the item-reading
    // helpers for it. Not duplicated here: this bundle has none, and adding one
    // to re-check a property another bundle already pins is how a fixture grows
    // a second reason to break.
}

// --------------------------------------------------------------- INV-11 -----

// A retitle leaves the store equal to its own regeneration. The failure this
// catches is silent: a stale slug means the re-import's findSection() misses
// and addSection() adds a SECOND section beside the first.
TEST(RoadmapRotateMinor, Inv11RetitleSurvivesReimport) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, twoMinorFixture(), &projectId);
    ASSERT_FALSE(root.isEmpty());

    const int sectionsBefore = sectionsOf(projectId).size();
    ASSERT_GT(sectionsBefore, 0);

    const QString oldSlug =
        RoadmapIndex::slugifyHeading(QString::fromUtf8(kClosedTitle));
    const QString newTitle =
        QString::fromUtf8("0.7.0 \xE2\x80\x94 first release \xE2\x80\x94 shipped (2026-01-02)");

    const QJsonObject resp = retitle(retitleReq(root, oldSlug, newTitle));
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    const QString newSlug = resp.value(QStringLiteral("slug")).toString();
    const auto idBefore = idOfSlug(projectId, newSlug);
    ASSERT_TRUE(idBefore.has_value());

    ASSERT_TRUE(reimport(root));

    EXPECT_EQ(sectionsOf(projectId).size(), sectionsBefore)
        << "a re-import that missed the section added a duplicate beside it";
    EXPECT_EQ(idOfSlug(projectId, newSlug), idBefore)
        << "the re-import must have UPDATED the section, not replaced it";
}

// --------------------------------------------------------------- INV-12 -----

// A retitle is refused when either direction of the slug-collision check fires
// — and the backward check is scoped to the retitled section's own family, so
// the two safe cases on the same fixture must succeed.
TEST(RoadmapRotateMinor, Inv12SlugCollisionBothDirections) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    // Two `## Performance` headings, so the walk gives the first `performance`
    // and the second `performance-2` — uniqueSlug()'s own worked example.
    QByteArray md = header();
    md += "## Alpha\n\n";
    md += bullet(kShipped, "DEMO-0001", "An alpha item.");
    md += "## Beta\n\n";
    md += bullet(kShipped, "DEMO-0002", "A beta item.");
    md += "## Performance\n\n";
    md += bullet(kShipped, "DEMO-0003", "A first performance item.");
    md += "## Performance\n\n";
    md += bullet(kShipped, "DEMO-0004", "A second performance item.");

    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, md, &projectId);
    ASSERT_FALSE(root.isEmpty());

    const QString head   = slugOfTitle(projectId, QStringLiteral("Performance"), 0);
    const QString member = slugOfTitle(projectId, QStringLiteral("Performance"), 1);
    ASSERT_EQ(head, QStringLiteral("performance"));
    ASSERT_EQ(member, QStringLiteral("performance-2"));

    // Forward — the new title slugifies onto a slug another section holds.
    {
        const QByteArray before = readAll(livePath(root));
        const QJsonObject resp = retitle(
            retitleReq(root, QStringLiteral("alpha"), QStringLiteral("Beta")));
        EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
        EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
                  QStringLiteral("bad_args"));
        EXPECT_TRUE(rowOfSlug(projectId, QStringLiteral("alpha")).has_value());
        EXPECT_TRUE(rowOfSlug(projectId, QStringLiteral("beta")).has_value());
        EXPECT_EQ(readAll(livePath(root)), before);
    }

    // Backward — retitling the un-suffixed HEAD frees `performance`, and the
    // next import would hand it to the `performance-2` section, changing the
    // slug of a section this call never touched.
    {
        const QByteArray before = readAll(livePath(root));
        const QJsonObject resp =
            retitle(retitleReq(root, head, QStringLiteral("Throughput")));
        EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
        EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
                  QStringLiteral("bad_args"));
        EXPECT_EQ(readAll(livePath(root)), before);
    }

    // Safe 1 — the suffixed MEMBER. The head keeps deriving the base, so
    // nothing is freed.
    {
        const QJsonObject resp =
            retitle(retitleReq(root, member, QStringLiteral("Latency")));
        EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
            << "refusing this is the check phrased over \"any section anywhere "
               "holds a disambiguated slug\": "
            << resp.value(QStringLiteral("code")).toString().toStdString();
        EXPECT_EQ(resp.value(QStringLiteral("slug")).toString(),
                  QStringLiteral("latency"));
    }

    // Safe 2 — an unrelated section, on a project that still holds a
    // disambiguated family.
    {
        const QJsonObject resp = retitle(
            retitleReq(root, QStringLiteral("beta"), QStringLiteral("Gamma")));
        EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
            << resp.value(QStringLiteral("code")).toString().toStdString();
        EXPECT_EQ(resp.value(QStringLiteral("slug")).toString(),
                  QStringLiteral("gamma"));
    }
}

// --------------------------------------------------------------- INV-13 -----

// A rotation leaves the store equal to its own regeneration. The importer
// prefixes every archive slug with its file's minor, so a rotation built as
// setSectionSource() alone leaves a slug no re-import derives.
TEST(RoadmapRotateMinor, Inv13RotationSurvivesReimport) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, twoMinorFixture(), &projectId);
    ASSERT_FALSE(root.isEmpty());

    // The move set, in the render's own document order — the order the next
    // import will walk the archive in, which is what makes the derived slugs
    // reproducible.
    QVector<RoadmapStore::SectionRow> moveSet;
    for (const auto &s : sectionsOf(projectId)) {
        if (s.title == QString::fromUtf8(kClosedTitle)
            || s.title == QString::fromUtf8(kChildTitle))
            moveSet.append(s);
    }
    ASSERT_EQ(moveSet.size(), 2);
    std::sort(moveSet.begin(), moveSet.end(), sectionOrderLess);

    QHash<QString, qint64> idsBefore;   // title → section_id
    for (const auto &s : moveSet) {
        const auto id = idOfSlug(projectId, s.slug);
        ASSERT_TRUE(id.has_value());
        idsBefore.insert(s.title, *id);
    }

    // The slugs the next import will derive: uniqueSlug() over the move set
    // ALONE, seeded empty, each prefixed with the archive's minor.
    QHash<QString, QString> expected;   // title → slug
    {
        QSet<QString> seen;
        for (const auto &s : moveSet)
            expected.insert(s.title, QStringLiteral("0-7-")
                                         + RoadmapIndex::uniqueSlug(seen, s.title));
    }

    const QJsonObject resp = rotate(rotateReq(root, QStringLiteral("0.7")));
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("code")).toString().toStdString() << ": "
        << resp.value(QStringLiteral("error")).toString().toStdString();

    for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
        SCOPED_TRACE(it.key().toStdString());
        const auto row = rowOfSlug(projectId, it.value());
        ASSERT_TRUE(row.has_value())
            << "expected stored slug " << it.value().toStdString();
        EXPECT_EQ(row->title, it.key());
        EXPECT_EQ(idOfSlug(projectId, it.value()), idsBefore.value(it.key()));
    }

    ASSERT_TRUE(reimport(root));

    // NOT a section count. The archive did not exist before this rotation, so
    // its own preamble — the level-0 section holding whatever precedes the
    // first `##` — is created by the FIRST re-import of the new file, and the
    // project legitimately gains one section. That is the render/import
    // contract, not rotation's, and a count cannot tell it from the duplicate
    // this invariant is about. Assert the duplicate directly instead: each
    // moved slug still resolves to the SAME section, and no moved heading is
    // carried by two.
    for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
        SCOPED_TRACE(it.key().toStdString());
        EXPECT_EQ(idOfSlug(projectId, it.value()), idsBefore.value(it.key()))
            << "the re-import derived a slug the store does not hold, so "
               "findSection() missed and addSection() replaced it"
            << dumpSections(projectId);
        int carriers = 0;
        for (const auto &s : sectionsOf(projectId))
            if (s.title == it.key()) ++carriers;
        EXPECT_EQ(carriers, 1)
            << "a duplicate heading beside the original — the silent half of "
               "this invariant, since both sections render"
            << dumpSections(projectId);
    }
}

// The live side frees the slugs it gives up, and that is refused, not absorbed.
TEST(RoadmapRotateMinor, Inv13LiveSideFreedBaseIsRefused) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    // `### Performance` under the CLOSED minor takes `performance`; the one
    // under the OPEN minor takes `performance-2`. Moving the first frees the
    // base, and the next import would re-derive `performance` for a section
    // rotation never touched.
    QByteArray md = header();
    md += QByteArray("## ") + kClosedTitle + "\n\n";
    md += bullet(kShipped, "DEMO-0001", "A shipped item.");
    md += "### Performance\n\n";
    md += bullet(kShipped, "DEMO-0002", "A closed performance item.");
    md += QByteArray("## ") + kOpenTitle + "\n\n";
    md += bullet(kPlanned, "DEMO-0003", "An open item.");
    md += "### Performance\n\n";
    md += bullet(kPlanned, "DEMO-0004", "An open performance item.");

    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, md, &projectId);
    ASSERT_FALSE(root.isEmpty());
    ASSERT_EQ(slugOfTitle(projectId, QStringLiteral("Performance"), 0),
              QStringLiteral("performance"));
    ASSERT_EQ(slugOfTitle(projectId, QStringLiteral("Performance"), 1),
              QStringLiteral("performance-2"));

    const QByteArray before = readAll(livePath(root));
    const QJsonObject resp = rotate(rotateReq(root, QStringLiteral("0.7")));
    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_args"));
    EXPECT_EQ(readAll(livePath(root)), before);
    EXPECT_FALSE(QFile::exists(archivePath(root)));
}
