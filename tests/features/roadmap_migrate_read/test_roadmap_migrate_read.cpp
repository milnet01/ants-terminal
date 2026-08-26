// Feature-conformance test for ANTS-3757 — the roadmap migration read half.
// Contract: docs/specs/ANTS-3757-roadmap-migration-read.md § 2.1 (the
// declarations) and § 3 (INV-1..13). Fixture and oracle notes:
// tests/features/roadmap_migrate_read/spec.md.
//
// Behavioural, against ants_core_lib. Committed fixtures carry the shapes that
// need a file on disk (discovery, the parity oracle, the INV-11 partition);
// the focused invariants drive inline documents so the case under test is
// visible beside its assertion.

#include <gtest/gtest.h>

#include "passheadingwrite.h"
#include "roadmapmigrate.h"
#include "roadmapparse.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QTemporaryDir>
#include <QStringList>

#include <algorithm>

using RoadmapMigrate::MigrationPlan;
using RoadmapMigrate::PlannedElement;
using RoadmapMigrate::PlannedItem;

namespace {

QString fixtureDir() { return QString::fromUtf8(ANTS_MIGRATE_FIXTURE_DIR); }

QString fixtureRoot(const char *name) {
    return fixtureDir() + QLatin1Char('/') + QString::fromUtf8(name);
}

QString fixtureText(const char *name) {
    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(fixtureRoot(name), &err);
    EXPECT_TRUE(disc.has_value()) << name << ": " << err.toStdString();
    if (!disc || disc->sources.isEmpty()) return QString();
    return disc->sources.constFirst().markdown;
}

// findRoadmaps + planFrom over a committed fixture, as migration itself runs.
MigrationPlan planFixture(const char *name) {
    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(fixtureRoot(name), &err);
    EXPECT_TRUE(disc.has_value()) << name << ": " << err.toStdString();
    if (!disc) return {};
    return RoadmapMigrate::planFrom(*disc, QString::fromUtf8(name),
                                    QString::fromUtf8(name));
}

// A single-source Discovery built by hand. ANTS-3766 § 2.1 moved `format` onto
// Source and findRoadmaps() is what fills it, so a hand-built Discovery has to
// set it too — planFrom() no longer detects anything, and leaving it empty
// would silently skip the pass-headings orphan scan.
RoadmapMigrate::Discovery inlineDiscovery(const QString &markdown,
                                          const QString &path) {
    RoadmapMigrate::Source s;
    s.path     = path;
    s.markdown = markdown;
    s.format   = RoadmapParse::detectRoadmapFormat(markdown.split(QLatin1Char('\n')));
    RoadmapMigrate::Discovery d;
    d.sources.append(s);
    return d;
}

MigrationPlan planText(const QString &markdown) {
    return RoadmapMigrate::planFrom(inlineDiscovery(markdown,
                                                    QStringLiteral("<inline>")),
                                    QStringLiteral("Inline"),
                                    QStringLiteral("inline"));
}

QList<int> noteLines(const MigrationPlan &plan, const char *code) {
    QList<int> out;
    for (const auto &n : plan.notes)
        if (n.code == QLatin1String(code)) out.append(n.line);
    return out;
}

int noteCount(const MigrationPlan &plan, const char *code) {
    return static_cast<int>(noteLines(plan, code).size());
}

const PlannedItem *itemAtLine(const MigrationPlan &plan, int line) {
    for (const auto &it : plan.items)
        if (it.firstLine == line) return &it;
    return nullptr;
}

QString prov(const PlannedItem &it, const char *field) {
    return it.provenance.value(QLatin1String(field)).toString();
}

QString extra(const PlannedItem &it, const char *field) {
    return it.extras.value(QLatin1String(field)).toString();
}

// One ants-v1 bullet, wrapped in the minimum a document needs.
QString v1Doc(const QString &bullets) {
    return QStringLiteral("## Active\n\n") + bullets;
}

// A pass-headings document carrying `value` as both blocks' Status. Two
// headings and two Status markers are detectRoadmapFormat()'s 2+2 threshold.
QString passDoc(const QString &value) {
    const QString line = value.isEmpty()
        ? QStringLiteral("")
        : QStringLiteral("- **Status**: ") + value + QLatin1Char('\n');
    return QStringLiteral("## Passes\n\n#### Pass 1.1 (LOW, S) One\n\n") + line +
           QStringLiteral("\n#### Pass 1.2 (LOW, S) Two\n\n") + line;
}

// A pass doc whose blocks carry NO Status line, but which still detects as
// pass-headings: the 2+2 threshold needs two Status markers somewhere, so they
// sit in a third block.
QString passDocNoStatus() {
    return QStringLiteral(
        "## Passes\n\n"
        "#### Pass 1.1 (LOW, S) No status here\n\n"
        "Some prose.\n\n"
        "#### Pass 1.2 (LOW, S) Nor here\n\n"
        "More prose.\n\n"
        "#### Pass 1.3 (LOW, S) But here\n\n"
        "- **Status**: done\n"
        "- **Status**: done\n");
}

// Everything a plan holds, in one string, so INV-9 can compare two runs
// field-wise and print what differs.
QString planDigest(const MigrationPlan &p) {
    QString d = p.projectName + '|' + p.exportSlug + '\n';
    for (const auto &src : p.sources)
        d += QStringLiteral("F %1|%2\n").arg(src.path, src.format);
    for (const auto &s : p.sections)
        d += QStringLiteral("S %1|%2|%3|%4|%5|%6-%7\n")
                 .arg(s.slug, s.title, s.intro, QString::number(s.level),
                      s.parentSlug, QString::number(s.firstLine))
                 .arg(s.lastLine) + QStringLiteral("@%1\n").arg(s.sourceIndex);
    for (const auto &i : p.items)
        d += QStringLiteral("I %1|%2|%3|%4|%5|%6|%7|%8|%9|")
                 .arg(i.id, i.idOrigin, i.status, i.headline, i.kind, i.source,
                      i.layman, i.body, i.sectionSlug) +
             i.lanes.join(',') + '|' + i.evidence.join(',') + '|' +
             QString::fromUtf8(QJsonDocument(i.extras).toJson(
                 QJsonDocument::Compact)) + '|' +
             QString::fromUtf8(QJsonDocument(i.provenance).toJson(
                 QJsonDocument::Compact)) +
             QStringLiteral("|%1|%2|%3|%4-%5\n")
                 .arg(i.position)
                 .arg(int(i.idAllocationOwed))
                 .arg(int(i.closed))
                 .arg(i.firstLine)
                 .arg(i.lastLine) + QStringLiteral("@%1\n").arg(i.sourceIndex);
    for (const auto &e : p.elements)
        d += QStringLiteral("E %1|%2|%3|%4|%5-%6\n")
                 .arg(e.kind, e.payload, e.sectionSlug,
                      QString::number(e.position), QString::number(e.firstLine))
                 .arg(e.lastLine) + QStringLiteral("@%1\n").arg(e.sourceIndex);
    if (p.legend)
        d += QStringLiteral("L %1|%2-%3\n")
                 .arg(QString::fromUtf8(QJsonDocument(p.legend->entries).toJson(
                          QJsonDocument::Compact)))
                 .arg(p.legend->firstLine)
                 .arg(p.legend->lastLine);
    for (const auto &n : p.notes)
        d += QStringLiteral("N %1|%2|%3|@%4\n")
                 .arg(n.code, n.detail).arg(n.line).arg(n.sourceIndex);
    return d;
}

// INV-11's partition, as a reusable check: every non-blank source line falls
// inside exactly one item, element, section or legend span. Returns a
// diagnostic, or an empty string when the plan partitions the source.
QString partitionFault(const QString &markdown, const MigrationPlan &plan) {
    const QStringList lines = markdown.split(QLatin1Char('\n'));
    QList<int> owners(lines.size() + 1, 0);
    QMap<int, QString> owner;
    const auto claim = [&](int first, int last, const QString &what) {
        for (int ln = first; ln <= last && ln <= lines.size(); ++ln) {
            if (ln < 1) continue;
            ++owners[ln];
            owner.insert(ln, owner.value(ln) + what + QLatin1Char(' '));
        }
    };
    for (const auto &s : plan.sections)
        claim(s.firstLine, s.lastLine, QStringLiteral("section:") + s.slug);
    for (const auto &i : plan.items)
        claim(i.firstLine, i.lastLine, QStringLiteral("item:") + i.headline);
    for (const auto &e : plan.elements)
        claim(e.firstLine, e.lastLine, QStringLiteral("element:") + e.kind);
    if (plan.legend)
        claim(plan.legend->firstLine, plan.legend->lastLine,
              QStringLiteral("legend"));

    for (int ln = 1; ln <= lines.size(); ++ln) {
        if (lines.at(ln - 1).trimmed().isEmpty()) continue;
        if (owners[ln] == 1) continue;
        return QStringLiteral("line %1 has %2 owners (%3): %4")
            .arg(ln).arg(owners[ln]).arg(owner.value(ln), lines.at(ln - 1));
    }
    // Every note with a line must resolve into one of those spans.
    for (const auto &n : plan.notes) {
        if (n.line == 0) continue;
        if (n.line >= 1 && n.line <= lines.size() && owners[n.line] > 0) continue;
        return QStringLiteral("note %1 names line %2, which is in no span")
            .arg(n.code).arg(n.line);
    }
    // One contiguous 0-based position sequence per section, items and
    // elements together.
    QMap<QString, QList<int>> perSection;
    for (const auto &i : plan.items) perSection[i.sectionSlug].append(i.position);
    for (const auto &e : plan.elements)
        perSection[e.sectionSlug].append(e.position);
    for (auto it = perSection.begin(); it != perSection.end(); ++it) {
        QList<int> pos = it.value();
        std::sort(pos.begin(), pos.end());
        for (int k = 0; k < pos.size(); ++k) {
            if (pos.at(k) == k) continue;
            return QStringLiteral("section '%1' position %2 at index %3")
                .arg(it.key()).arg(pos.at(k)).arg(k);
        }
    }
    return QString();
}

const char *kPartitionFixtures[] = {"antsv1", "gfm", "passes", "identity",
                                    "malformed", "prose", "empty"};


// --- ANTS-3766: archives as additional sources --------------------------------

QString archiveRoot(const char *name) {
    return fixtureDir() + QStringLiteral("/archives/") + QString::fromUtf8(name);
}

std::optional<RoadmapMigrate::Discovery> discover(const char *name, QString *err) {
    return RoadmapMigrate::findRoadmaps(archiveRoot(name), err);
}

MigrationPlan planArchiveRoot(const char *name) {
    QString err;
    const auto disc = discover(name, &err);
    EXPECT_TRUE(disc.has_value()) << name << ": " << err.toStdString();
    if (!disc) return {};
    return RoadmapMigrate::planFrom(*disc, QString::fromUtf8(name),
                                    QString::fromUtf8(name));
}

QStringList slugsForSource(const MigrationPlan &plan, int sourceIndex) {
    QStringList out;
    for (const auto &s : plan.sections)
        if (s.sourceIndex == sourceIndex) out.append(s.slug);
    return out;
}

QStringList noteDetails(const MigrationPlan &plan, const char *code) {
    QStringList out;
    for (const auto &n : plan.notes)
        if (n.code == QLatin1String(code)) out.append(n.detail);
    out.sort();
    return out;
}

QStringList sourceFileNames(const MigrationPlan &plan) {
    QStringList out;
    for (const auto &s : plan.sources) out.append(QFileInfo(s.path).fileName());
    return out;
}

// ANTS-3766 INV-5 — INV-11's partition, now PER SOURCE: every non-blank line of
// source i lies inside exactly one carrier whose sourceIndex == i.
QString partitionFaultForSource(const MigrationPlan &plan, int sourceIndex) {
    const QStringList lines =
        plan.sources.at(sourceIndex).markdown.split(QLatin1Char('\n'));
    QList<int> owners(lines.size() + 1, 0);
    QMap<int, QString> owner;
    const auto claim = [&](int first, int last, const QString &what) {
        for (int ln = first; ln <= last && ln <= lines.size(); ++ln) {
            if (ln < 1) continue;
            ++owners[ln];
            owner.insert(ln, owner.value(ln) + what + QLatin1Char(' '));
        }
    };
    for (const auto &s : plan.sections)
        if (s.sourceIndex == sourceIndex)
            claim(s.firstLine, s.lastLine, QStringLiteral("section:") + s.slug);
    for (const auto &i : plan.items)
        if (i.sourceIndex == sourceIndex)
            claim(i.firstLine, i.lastLine, QStringLiteral("item:") + i.headline);
    for (const auto &e : plan.elements)
        if (e.sourceIndex == sourceIndex)
            claim(e.firstLine, e.lastLine, QStringLiteral("element:") + e.kind);
    if (plan.legend && plan.legend->sourceIndex == sourceIndex)
        claim(plan.legend->firstLine, plan.legend->lastLine,
              QStringLiteral("legend"));

    for (int ln = 1; ln <= lines.size(); ++ln) {
        if (lines.at(ln - 1).trimmed().isEmpty()) continue;
        if (owners[ln] == 1) continue;
        return QStringLiteral("source %1 line %2 has %3 owners (%4): %5")
            .arg(sourceIndex).arg(ln).arg(owners[ln])
            .arg(owner.value(ln), lines.at(ln - 1));
    }
    return QString();
}

// A root copied to a temp directory, so INV-4's live-file edit never touches a
// committed fixture: editing one in place corrupts every later assertion in the
// same run and shows up as a dirty working tree.
struct TempRoot {
    QTemporaryDir dir;
    QString root;
    explicit TempRoot(const char *name) {
        root = dir.path() + QStringLiteral("/root");
        const QString src = archiveRoot(name);
        QDirIterator walk(src, QDir::Files | QDir::Hidden,
                          QDirIterator::Subdirectories);
        while (walk.hasNext()) {
            const QString from = walk.next();
            const QString rel  = QDir(src).relativeFilePath(from);
            const QString to   = root + QLatin1Char('/') + rel;
            QDir().mkpath(QFileInfo(to).absolutePath());
            QFile::copy(from, to);
        }
    }
    void appendToLiveRoadmap(const QString &text) const {
        QFile f(root + QStringLiteral("/ROADMAP.md"));
        EXPECT_TRUE(f.open(QIODevice::Append | QIODevice::Text));
        f.write(text.toUtf8());
    }
    MigrationPlan plan() const {
        QString err;
        const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
        EXPECT_TRUE(disc.has_value()) << err.toStdString();
        if (!disc) return {};
        return RoadmapMigrate::planFrom(*disc, QStringLiteral("T"),
                                        QStringLiteral("t"));
    }
};

}  // namespace

// ---------------------------------------------------------------- INV-1 ----
// Discovery per § 2.2: case-insensitive, non-recursive, three refusals
// asserted on their code rather than on a human message.
TEST(roadmap_migrate_read, Inv1Discovery) {
    QString err;
    const auto upper = RoadmapMigrate::findRoadmaps(fixtureRoot("discovery/upper"), &err);
    ASSERT_TRUE(upper.has_value()) << err.toStdString();
    ASSERT_FALSE(upper->sources.isEmpty());
    EXPECT_TRUE(upper->sources.constFirst().markdown.contains(QStringLiteral("UP-0001")))
        << "INV-1: the file's own text is returned, not just its path";
    EXPECT_TRUE(upper->sources.constFirst().path.endsWith(QStringLiteral("ROADMAP.md")));

    err.clear();
    const auto lower = RoadmapMigrate::findRoadmaps(fixtureRoot("discovery/lower"), &err);
    ASSERT_TRUE(lower.has_value()) << err.toStdString();
    ASSERT_FALSE(lower->sources.isEmpty());
    EXPECT_TRUE(lower->sources.constFirst().markdown.contains(QStringLiteral("LOW-0001")))
        << "INV-1: an uppercase-only glob drops the one project that owns the "
           "whole pass corpus";
    EXPECT_TRUE(lower->sources.constFirst().path.endsWith(QStringLiteral("roadmap.md")));

    err.clear();
    EXPECT_FALSE(RoadmapMigrate::findRoadmaps(fixtureRoot("discovery/none"), &err));
    EXPECT_EQ(err, QStringLiteral("not_found"));

    err.clear();
    EXPECT_FALSE(RoadmapMigrate::findRoadmaps(fixtureRoot("discovery/both"), &err));
    EXPECT_EQ(err, QStringLiteral("case_ambiguous"))
        << "INV-1: either choice silently discards a whole project's roadmap";

    err.clear();
    EXPECT_FALSE(RoadmapMigrate::findRoadmaps(fixtureRoot("discovery/badutf8"), &err));
    EXPECT_EQ(err, QStringLiteral("not_utf8"))
        << "INV-1: a lossy U+FFFD decode leaves INV-11 green over corrupted "
           "content";

    err.clear();
    EXPECT_FALSE(RoadmapMigrate::findRoadmaps(fixtureRoot("discovery/absent"), &err));
    EXPECT_EQ(err, QStringLiteral("not_found"));
}

// ANTS-4693 — the declared `roadmap` key. roadmap_query and roadmap_log both
// honour `.ants/project.json`, and this verb did not — so a project keeping
// its roadmap anywhere but the root could be READ and WRITTEN but not ADOPTED,
// by the one verb that decides whether it can use the store at all. The
// reporter isolated it with a symlink: ROADMAP.md -> docs/11-roadmap.md, and
// with nothing else changed the same migration succeeded.
TEST(roadmap_migrate_read, Ants4693HonoursTheDeclaredRoadmapKey) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = tmp.path();
    ASSERT_TRUE(QDir(root).mkpath(QStringLiteral("docs")));
    ASSERT_TRUE(QDir(root).mkpath(QStringLiteral(".ants")));

    const auto put = [&](const QString &rel, const QByteArray &body) {
        QFile f(root + QLatin1Char('/') + rel);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        const bool ok = f.write(body) == body.size();
        f.close();
        return ok;
    };

    ASSERT_TRUE(put(QStringLiteral("docs/11-roadmap.md"),
        "# Roadmap\n\n## Work\n\n"
        "- \xF0\x9F\x93\x8B [PERCH-0001] **A declared-path bullet.**\n"
        "  Kind: fix.\n  Source: seed.\n"));
    ASSERT_TRUE(put(QStringLiteral(".ants/project.json"),
                    "{\"roadmap\": \"docs/11-roadmap.md\"}"));
    // No root-level roadmap at all: without the declared key there is nothing
    // to find, which is what makes this assert the key rather than the scan.
    ASSERT_FALSE(QFileInfo::exists(root + QStringLiteral("/ROADMAP.md")));

    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    ASSERT_TRUE(disc.has_value())
        << "the declared roadmap must be discoverable: " << err.toStdString();
    ASSERT_FALSE(disc->sources.isEmpty());
    EXPECT_TRUE(disc->sources.constFirst().path.endsWith(
        QStringLiteral("docs/11-roadmap.md")))
        << "got: " << disc->sources.constFirst().path.toStdString();
    EXPECT_TRUE(disc->sources.constFirst().markdown.contains(
        QStringLiteral("PERCH-0001")))
        << "the file's own text is returned, not just its path";
}

// ANTS-4693 — a declaration naming a file that is not there falls through to
// the ordinary root scan. An unusable declaration is not evidence that the
// root file is absent, and refusing on it would make a stale settings key
// break a project that is otherwise fine.
TEST(roadmap_migrate_read, Ants4693StaleDeclarationFallsBackToTheScan) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = tmp.path();
    ASSERT_TRUE(QDir(root).mkpath(QStringLiteral(".ants")));

    const auto put = [&](const QString &rel, const QByteArray &body) {
        QFile f(root + QLatin1Char('/') + rel);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        const bool ok = f.write(body) == body.size();
        f.close();
        return ok;
    };

    ASSERT_TRUE(put(QStringLiteral("ROADMAP.md"),
        "# Roadmap\n\n## Work\n\n"
        "- \xF0\x9F\x93\x8B [ROOT-0001] **The ordinary root bullet.**\n"
        "  Kind: fix.\n  Source: seed.\n"));
    ASSERT_TRUE(put(QStringLiteral(".ants/project.json"),
                    "{\"roadmap\": \"docs/gone.md\"}"));

    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    ASSERT_TRUE(disc.has_value())
        << "a stale declaration must not take the project down: "
        << err.toStdString();
    ASSERT_FALSE(disc->sources.isEmpty());
    EXPECT_TRUE(disc->sources.constFirst().path.endsWith(
        QStringLiteral("ROADMAP.md")));
    EXPECT_TRUE(disc->sources.constFirst().markdown.contains(
        QStringLiteral("ROOT-0001")));
}

// ---------------------------------------------------------------- INV-2 ----
// The item count matches an independently written parser's, per fixture.
TEST(roadmap_migrate_read, Inv2ItemCountParity) {
    QFile f(QString::fromUtf8(ANTS_MIGRATE_EXPECTED_JSON));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly)) << "expected-counts.json unreadable";
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    const QJsonObject expected = root.value(QStringLiteral("items")).toObject();
    ASSERT_FALSE(expected.isEmpty());

    for (auto it = expected.begin(); it != expected.end(); ++it) {
        const QByteArray name = it.key().toUtf8();
        const MigrationPlan plan = planFixture(name.constData());
        EXPECT_EQ(plan.items.size(), it.value().toInt())
            << "INV-2: fixture '" << name.constData()
            << "' disagrees with the survey oracle. Regenerating the "
               "expectation file to make this pass is forbidden";
    }
}

// ---------------------------------------------------------------- INV-3 ----
// Identity is decided by position, and an item with no id carries the
// allocation obligation with its open/closed sense.
TEST(roadmap_migrate_read, Inv3IdentityIsPositional) {
    const MigrationPlan plan = planFixture("identity");
    ASSERT_EQ(plan.items.size(), 7);

    const PlannedItem *cl9 = itemAtLine(plan, 8);
    ASSERT_NE(cl9, nullptr);
    EXPECT_EQ(cl9->id, QStringLiteral("Cl9"))
        << "INV-3: the only leading-slot token here that is an id";

    // The four that must NOT yield an id: a markdown link in the leading slot,
    // and three id-shaped tokens that are references, not declarations.
    for (int line : {9, 10, 11, 14}) {
        const PlannedItem *it = itemAtLine(plan, line);
        ASSERT_NE(it, nullptr) << "line " << line;
        EXPECT_TRUE(it->id.isEmpty())
            << "INV-3: line " << line << " planned id '"
            << it->id.toStdString()
            << "' — position is the whole discriminator, and a leading-slot "
               "token followed by `(` or `:` is a markdown link";
        EXPECT_TRUE(it->idAllocationOwed);
        EXPECT_EQ(prov(*it, "id"), QStringLiteral("migrated"));
    }
    // ANTS-4708 — ASSERT each lookup before dereferencing it. `closedNoId`
    // was already guarded and its two neighbours were not, so a fixture edit
    // that moved either line would SEGFAULT the bundle instead of failing:
    // the suite would report that the binary died rather than which invariant
    // broke, and under a parallel run the surviving output is whichever test
    // happened to be writing. Telling you which item moved is the whole value
    // of these plan tests.
    const PlannedItem *openAt9  = itemAtLine(plan, 9);
    const PlannedItem *openAt11 = itemAtLine(plan, 11);
    const PlannedItem *closedNoId = itemAtLine(plan, 14);
    ASSERT_NE(openAt9, nullptr)   << "no item spans line 9";
    ASSERT_NE(openAt11, nullptr)  << "no item spans line 11";
    ASSERT_NE(closedNoId, nullptr) << "no item spans line 14";
    EXPECT_FALSE(openAt9->closed);
    EXPECT_FALSE(openAt11->closed);
    EXPECT_TRUE(closedNoId->closed)
        << "INV-3: closed matches the item's own status";

    // The GFM leg: the reader invents a content-hash id for an id-less
    // checkbox item. roadmap-data-model.md § 7.2 must not see it, or the item
    // silently leaves the id-less population its bulk allocation serves.
    const MigrationPlan gfm = planFixture("gfm");
    ASSERT_EQ(gfm.items.size(), 3);
    const PlannedItem *noId = itemAtLine(gfm, 6);
    ASSERT_NE(noId, nullptr);
    EXPECT_TRUE(noId->id.isEmpty())
        << "INV-3: a reader-`synthetic` id is discarded, not taken at face "
           "value";
    EXPECT_TRUE(noId->idAllocationOwed);
    const PlannedItem *withId = itemAtLine(gfm, 5);
    ASSERT_NE(withId, nullptr);
    EXPECT_EQ(withId->id, QStringLiteral("DONE-1"));
    EXPECT_FALSE(withId->idAllocationOwed);
    EXPECT_EQ(noteCount(gfm, "id_allocation_owed"), 1);
}

// ---------------------------------------------------------------- INV-4 ----
// Quarantine imports the id verbatim, bracket-free and unrepaired.
TEST(roadmap_migrate_read, Inv4Quarantine) {
    const MigrationPlan plan = planFixture("identity");
    const PlannedItem *cl9 = itemAtLine(plan, 8);
    ASSERT_NE(cl9, nullptr);
    EXPECT_EQ(cl9->id, QStringLiteral("Cl9"))
        << "INV-4: verbatim — never repaired to `CL-9`, which would break "
           "roadmap-format.md § 3.5.1's append-only rule";
    EXPECT_EQ(cl9->idOrigin, QStringLiteral("quarantined"));
    EXPECT_FALSE(cl9->idAllocationOwed)
        << "INV-4: treating it as id-less issues a second identity for an "
           "item that already has one";
    EXPECT_EQ(noteLines(plan, "quarantined_id"), QList<int>({8}));

    // The shape the field's whole justification rests on, and the one a
    // fixture holding `Cl9` alone leaves untested: the reader refuses
    // `[ANTS-119&]` outright where it accepts `Cl9`, so without the raw token
    // this item reaches migration id-less.
    const MigrationPlan bad = planFixture("malformed");
    ASSERT_EQ(bad.items.size(), 1);
    EXPECT_EQ(bad.items[0].id, QStringLiteral("ANTS-119&"));
    EXPECT_EQ(bad.items[0].idOrigin, QStringLiteral("quarantined"));
    EXPECT_FALSE(bad.items[0].idAllocationOwed);
    EXPECT_EQ(noteLines(bad, "quarantined_id"), QList<int>({8}));

    // A conforming id is not quarantined.
    EXPECT_EQ(itemAtLine(plan, 12)->idOrigin, QStringLiteral("parsed"));
}

// ---------------------------------------------------------------- INV-5 ----
// Every source shape maps to the status § 2.7's tables give it, per shape and
// not merely in aggregate; `dropped` is never produced.
TEST(roadmap_migrate_read, Inv5StatusPerSourceShape) {
    const QList<QPair<QString, QString>> emoji = {
        {QStringLiteral("✅"), QStringLiteral("shipped")},
        {QStringLiteral("🚧"), QStringLiteral("in-progress")},
        {QStringLiteral("📋"), QStringLiteral("planned")},
        {QStringLiteral("💭"), QStringLiteral("considered")},
    };
    for (const auto &row : emoji) {
        const MigrationPlan p = planText(
            v1Doc(QStringLiteral("- ") + row.first +
                  QStringLiteral(" [X-0001] **A headline.**\n")));
        ASSERT_EQ(p.items.size(), 1) << row.first.toStdString();
        EXPECT_EQ(p.items[0].status, row.second)
            << "INV-5: emoji " << row.first.toStdString();
    }

    const MigrationPlan gfm = planText(QStringLiteral(
        "## Backlog\n\n- [x] **A-1** — done\n- [ ] **A-2** — not done\n"));
    ASSERT_EQ(gfm.items.size(), 2);
    EXPECT_EQ(gfm.items[0].status, QStringLiteral("shipped"));
    EXPECT_EQ(gfm.items[1].status, QStringLiteral("planned"));

    const QList<QPair<QString, QString>> words = {
        {QStringLiteral("done"), QStringLiteral("shipped")},
        {QStringLiteral("shipped"), QStringLiteral("shipped")},
        {QStringLiteral("completed"), QStringLiteral("shipped")},
        {QStringLiteral("in-progress"), QStringLiteral("in-progress")},
        {QStringLiteral("in_progress"), QStringLiteral("in-progress")},
        {QStringLiteral("inprogress"), QStringLiteral("in-progress")},
        {QStringLiteral("doing"), QStringLiteral("in-progress")},
        {QStringLiteral("wip"), QStringLiteral("in-progress")},
        {QStringLiteral("deferred"), QStringLiteral("considered")},
        {QStringLiteral("considered"), QStringLiteral("considered")},
        {QStringLiteral("parked"), QStringLiteral("considered")},
        {QStringLiteral("todo"), QStringLiteral("planned")},
        {QStringLiteral("planned"), QStringLiteral("planned")},
        {QStringLiteral("frobnicated"), QStringLiteral("planned")},
        // Migration inherits the reader's matching and adds none of its own:
        // it already folds case and absorbs a leading `*` run.
        {QStringLiteral("Done"), QStringLiteral("shipped")},
        {QStringLiteral("**deferred**"), QStringLiteral("considered")},
    };
    for (const auto &row : words) {
        const MigrationPlan p = planText(passDoc(row.first));
        ASSERT_EQ(p.items.size(), 2) << row.first.toStdString();
        EXPECT_EQ(p.items[0].status, row.second)
            << "INV-5: Status word '" << row.first.toStdString() << "'";
    }

    // The round trip a later roadmap_log flip depends on.
    for (const QString &status :
         {QStringLiteral("planned"), QStringLiteral("in-progress"),
          QStringLiteral("shipped"), QStringLiteral("considered")}) {
        const QString kw = PassHeadingWrite::passStatusKeyword(status);
        ASSERT_FALSE(kw.isEmpty()) << status.toStdString();
        const MigrationPlan p = planText(passDoc(kw));
        ASSERT_EQ(p.items.size(), 2);
        EXPECT_EQ(p.items[0].status, status)
            << "INV-5: status → keyword → status must close for '"
            << status.toStdString() << "'; mapping `deferred` to `planned` "
               "breaks it";
    }

    for (const char *name : kPartitionFixtures)
        for (const auto &it : planFixture(name).items)
            EXPECT_NE(it.status, QStringLiteral("dropped"))
                << "INV-5: no markdown serialisation can express `dropped`";
}

// ---------------------------------------------------------------- INV-6 ----
// A named word is a transcription (`asserted`); an unnamed word or an absent
// Status line is a guess (`defaulted`) and is reported.
TEST(roadmap_migrate_read, Inv6StatusProvenance) {
    // ANTS-4071 moved `partial` out of this group and into the named one
    // below. `un-gated` is now the only word in the surveyed corpus that takes
    // the default — correctly, per roadmap-data-model.md § 7.3.1's own
    // measurement: that item's blocking gate had been met and the work had not
    // begun. One word is enough to hold the invariant, and it is the real one.
    for (const QString &word : {QStringLiteral("un-gated")}) {
        const MigrationPlan p = planText(passDoc(word));
        ASSERT_EQ(p.items.size(), 2);
        EXPECT_EQ(p.items[0].status, QStringLiteral("planned"));
        EXPECT_EQ(prov(p.items[0], "status"), QStringLiteral("defaulted"))
            << "INV-6: '" << word.toStdString()
            << "' is migration's guess, not the author's choice";
        EXPECT_EQ(noteCount(p, "status_defaulted"), 2);
    }

    const MigrationPlan none = planText(passDocNoStatus());
    ASSERT_EQ(none.items.size(), 3);
    EXPECT_EQ(none.items[0].status, QStringLiteral("planned"));
    EXPECT_EQ(prov(none.items[0], "status"), QStringLiteral("defaulted"));
    EXPECT_EQ(prov(none.items[1], "status"), QStringLiteral("defaulted"));
    EXPECT_EQ(prov(none.items[2], "status"), QStringLiteral("asserted"));
    EXPECT_EQ(noteCount(none, "status_defaulted"), 2)
        << "INV-6: a block with no Status line at all is defaulted and "
           "reported";

    for (const QString &word : {QStringLiteral("done"), QStringLiteral("todo"),
                                QStringLiteral("deferred"),
                                // ANTS-4071 — named as of 2026-08-09.
                                QStringLiteral("partial")}) {
        const MigrationPlan p = planText(passDoc(word));
        EXPECT_EQ(prov(p.items[0], "status"), QStringLiteral("asserted"))
            << "INV-6: '" << word.toStdString()
            << "' — only the notation differs from an author writing an emoji";
        EXPECT_EQ(noteCount(p, "status_defaulted"), 0);
    }
    EXPECT_EQ(planText(passDoc(QStringLiteral("partial"))).items[0].status,
              QStringLiteral("in-progress"))
        << "ANTS-4071: `partial` means begun; importing it as planned says the "
           "opposite about work a reader would act on";

    const MigrationPlan v1 = planText(
        v1Doc(QStringLiteral("- ✅ [X-0001] **Emoji.**\n")));
    EXPECT_EQ(prov(v1.items[0], "status"), QStringLiteral("asserted"));
    EXPECT_EQ(noteCount(v1, "status_defaulted"), 0);
    const MigrationPlan gfm = planText(
        QStringLiteral("## B\n\n- [x] **A-1** — done\n"));
    EXPECT_EQ(prov(gfm.items[0], "status"), QStringLiteral("asserted"));
}

// ---------------------------------------------------------------- INV-7 ----
// extras.source_status holds the verbatim Status VALUE, and nothing else sets
// it.
TEST(roadmap_migrate_read, Inv7SourceStatusVerbatim) {
    const MigrationPlan tail = planText(
        passDoc(QStringLiteral("done (v3.20.0, 2026-07-05). Adds catalogs for x")));
    ASSERT_EQ(tail.items.size(), 2);
    EXPECT_EQ(extra(tail.items[0], "source_status"),
              QStringLiteral("done (v3.20.0, 2026-07-05). Adds catalogs for x"))
        << "INV-7: the whole value, not the matched word — the qualifier tail "
           "is what made preservation worth doing";

    const MigrationPlan completed = planText(passDoc(QStringLiteral("completed")));
    EXPECT_EQ(extra(completed.items[0], "source_status"),
              QStringLiteral("completed"))
        << "INV-7: not the `done` its round trip would write back";

    const MigrationPlan starred =
        planText(passDoc(QStringLiteral("**un-gated (2026-07-05).**")));
    EXPECT_EQ(extra(starred.items[0], "source_status"),
              QStringLiteral("**un-gated (2026-07-05).**"))
        << "INV-7: matching strips a leading `*`; storage strips nothing";

    const MigrationPlan noStatus = planText(passDocNoStatus());
    EXPECT_FALSE(noStatus.items[0].extras.contains(QStringLiteral("source_status")))
        << "INV-7: a pass block with no Status line sets it not at all";

    const MigrationPlan v1 = planText(
        v1Doc(QStringLiteral("- ✅ [X-0001] **Emoji.**\n")));
    EXPECT_FALSE(v1.items[0].extras.contains(QStringLiteral("source_status")))
        << "INV-7: the marker itself is the status on the emoji path";
}

// ---------------------------------------------------------------- INV-8 ----
// Kind and Source per § 2.8: defaulted when absent, mapped when non-canonical,
// reported when unmapped, and never a refusal.
TEST(roadmap_migrate_read, Inv8KindAndSource) {
    const MigrationPlan plan = planFixture("antsv1");
    const PlannedItem *mapped = itemAtLine(plan, 16);
    ASSERT_NE(mapped, nullptr);
    EXPECT_EQ(mapped->kind, QStringLiteral("fix"))
        << "INV-8: roadmap-data-model.md § 7.4 maps `bugfix` → `fix`";
    EXPECT_EQ(prov(*mapped, "kind"), QStringLiteral("asserted"));
    EXPECT_EQ(extra(*mapped, "source_kind"), QStringLiteral("bugfix"))
        << "INV-8: the author's verbatim value, kept whenever the canonical "
           "kind differs from what the source said";
    EXPECT_EQ(mapped->source, QStringLiteral("in-session-2026-07-31"));
    EXPECT_EQ(prov(*mapped, "source"), QStringLiteral("asserted"));
    EXPECT_EQ(mapped->layman, QStringLiteral("it will do the thing"));
    EXPECT_EQ(mapped->lanes, QStringList({QStringLiteral("docs"),
                                          QStringLiteral("parser")}));
    EXPECT_EQ(mapped->evidence, QStringList({QStringLiteral("logs/run.log"),
                                             QStringLiteral("logs/second.log")}));

    const PlannedItem *canonical = itemAtLine(plan, 23);
    ASSERT_NE(canonical, nullptr);
    EXPECT_EQ(canonical->kind, QStringLiteral("implement"));
    EXPECT_FALSE(canonical->extras.contains(QStringLiteral("source_kind")))
        << "INV-8: a canonical value written by the author sets neither extras "
           "nor a non-asserted provenance";

    const PlannedItem *bare = itemAtLine(plan, 39);
    ASSERT_NE(bare, nullptr);
    EXPECT_EQ(bare->kind, QStringLiteral("implement"));
    EXPECT_EQ(prov(*bare, "kind"), QStringLiteral("defaulted"))
        << "INV-8: refusing an item with no Kind: refuses half the corpus";
    EXPECT_EQ(bare->source, QStringLiteral("planned"));
    EXPECT_EQ(prov(*bare, "source"), QStringLiteral("defaulted"));

    const MigrationPlan unmapped = planText(v1Doc(QStringLiteral(
        "- 📋 [X-0001] **Unmapped kind.**\n  Kind: frobnicate.\n")));
    ASSERT_EQ(unmapped.items.size(), 1);
    EXPECT_EQ(unmapped.items[0].kind, QStringLiteral("implement"))
        << "INV-8: reported and defaulted, never refused — the table was "
           "generated from a corpus that grows";
    EXPECT_EQ(prov(unmapped.items[0], "kind"), QStringLiteral("defaulted"));
    EXPECT_EQ(extra(unmapped.items[0], "source_kind"),
              QStringLiteral("frobnicate"));
    EXPECT_EQ(noteCount(unmapped, "kind_unmapped"), 1);
}

// ---------------------------------------------------------------- INV-9 ----
// planFrom() is pure: same input, same plan, and no filesystem touched.
TEST(roadmap_migrate_read, Inv9Purity) {
    // The fixture is load-bearing and `identity` is the one that works.
    // Measured 2026-07-31: against `antsv1`, whose every item declares an id,
    // the mutation § 2.9 exists to prevent — allocate here, reading AND
    // incrementing a counter — never reaches its branch and this invariant
    // stays green while broken. `identity` carries four id-less items.
    const QString markdown = fixtureText("identity");
    ASSERT_FALSE(markdown.isEmpty());

    QMap<QString, QDateTime> before;
    QDirIterator walk(fixtureDir(), QDir::Files, QDirIterator::Subdirectories);
    while (walk.hasNext()) {
        const QString p = walk.next();
        before.insert(p, QFileInfo(p).lastModified());
    }
    ASSERT_FALSE(before.isEmpty());

    const RoadmapMigrate::Discovery disc =
        inlineDiscovery(markdown, QStringLiteral("/x/ROADMAP.md"));
    const MigrationPlan first =
        RoadmapMigrate::planFrom(disc, QStringLiteral("P"), QStringLiteral("p"));
    const MigrationPlan second =
        RoadmapMigrate::planFrom(disc, QStringLiteral("P"), QStringLiteral("p"));
    EXPECT_EQ(planDigest(first), planDigest(second))
        << "INV-9: an id counter read AND incremented here is what this "
           "detects; allocation belongs to ANTS-3765";

    for (auto it = before.begin(); it != before.end(); ++it)
        EXPECT_EQ(QFileInfo(it.key()).lastModified(), it.value())
            << "INV-9: planFrom touched " << it.key().toStdString();
}

// --------------------------------------------------------------- INV-10 ----
// Pass blocks: one item each, the reader's synthesised id, the first
// CLASSIFYING Status line, and a Status line in no block reported.
TEST(roadmap_migrate_read, Inv10PassBlocks) {
    const MigrationPlan plan = planFixture("passes");
    ASSERT_EQ(plan.items.size(), 3) << "INV-10: one item per block";
    ASSERT_FALSE(plan.sources.isEmpty());
    EXPECT_EQ(plan.sources.constFirst().format, QStringLiteral("pass-headings"));

    EXPECT_EQ(plan.items[0].id,
              PassHeadingWrite::passIdFromDesignator(QStringLiteral("43.5")))
        << "INV-10: reader and writer must agree or roadmap_log stops finding "
           "what it wrote";
    EXPECT_EQ(plan.items[1].id,
              PassHeadingWrite::passIdFromDesignator(QStringLiteral("43.5.B")))
        << "INV-10: dropping the letter-led sub-designator collapses 43.5 and "
           "43.5.B onto one id — the regression ANTS-2035 already fixed once";
    EXPECT_NE(plan.items[0].id, plan.items[1].id);
    for (const auto &it : plan.items)
        EXPECT_EQ(it.idOrigin, QStringLiteral("synthesised"));

    // Block 43.5 carries a content-free Status line, then `partial`, then
    // `done`. The reader takes the first that CLASSIFIES.
    //
    // ANTS-4071 — `partial` is in-progress, not planned. It used to fall to the
    // reader's else-branch and import as 📋, which says "not started" about work
    // whose own Status line says a phase of it landed; a reader would act on
    // that. Named rather than guessed, so provenance is `asserted` and the
    // block raises no `status_defaulted` note.
    EXPECT_EQ(plan.items[0].status, QStringLiteral("in-progress"))
        << "INV-10: the first classifying line wins; the last would make this "
           "shipped";
    EXPECT_EQ(extra(plan.items[0], "source_status"),
              QStringLiteral("partial (v3.6.20). Phase A landed"));
    EXPECT_EQ(plan.items[1].status, QStringLiteral("considered"));

    EXPECT_EQ(noteLines(plan, "orphan_status_line"), QList<int>({3}))
        << "INV-10: a Status line belonging to no pass block is reported, not "
           "imported and not dropped";
    // Only Pass 44.1, which carries no Status line at all. Was 2 before
    // ANTS-4071 named `partial`.
    EXPECT_EQ(noteCount(plan, "status_defaulted"), 1);
}

// --------------------------------------------------------------- INV-11 ----
// No source content is silently discarded: the four span-bearing carriers
// partition every non-blank line, notes resolve into a span, and each
// section's positions are one contiguous 0-based sequence.
TEST(roadmap_migrate_read, Inv11NothingIsDiscarded) {
    for (const char *name : kPartitionFixtures) {
        const QString markdown = fixtureText(name);
        const MigrationPlan plan = planFixture(name);
        EXPECT_EQ(partitionFault(markdown, plan), QString())
            << "INV-11: fixture '" << name << "'";
    }

    // The carriers the partition needs are actually populated: against a plan
    // holding only items and notes the invariant is unsatisfiable rather than
    // merely unmet.
    const MigrationPlan v1 = planFixture("antsv1");
    EXPECT_FALSE(v1.sections.isEmpty());
    EXPECT_FALSE(v1.elements.isEmpty());
    EXPECT_TRUE(v1.legend.has_value())
        << "INV-11: without a legend carrier the legend lines are lost, or "
           "promoted to items under INV-2's own mutation";
    EXPECT_EQ(v1.legend->firstLine, 7);
    EXPECT_EQ(v1.legend->lastLine, 10);
    EXPECT_EQ(v1.legend->entries.value(QStringLiteral("shipped")).toString(),
              QStringLiteral("Done (shipped and released)"));
    EXPECT_EQ(v1.legend->entries.value(QStringLiteral("considered")).toString(),
              QStringLiteral("Considered (an idea, not a commitment)"));

    // A table is an element whose separator row is delimiter, not content.
    const PlannedElement *table = nullptr;
    for (const auto &e : v1.elements)
        if (e.kind == QLatin1String("table")) table = &e;
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(table->firstLine, 34);
    EXPECT_EQ(table->lastLine, 37);
    const QJsonObject payload =
        QJsonDocument::fromJson(table->payload.toUtf8()).object();
    EXPECT_EQ(payload.value(QStringLiteral("header")).toArray().size(), 2);
    EXPECT_EQ(payload.value(QStringLiteral("rows")).toArray().size(), 2)
        << "INV-11: the separator row is never stored";

    // Sections: the synthetic root, and `###` nesting under its `##`.
    bool sawRoot = false, sawNested = false;
    for (const auto &s : v1.sections) {
        if (s.level == 0 && s.slug.isEmpty()) sawRoot = true;
        if (s.level == 3) {
            sawNested = true;
            EXPECT_EQ(s.parentSlug, QStringLiteral("active"));
        }
    }
    EXPECT_TRUE(sawRoot) << "INV-11: content before the first heading";
    EXPECT_TRUE(sawNested);
}

// --------------------------------------------------------------- INV-12 ----
// Two items whose ids fold together are kept, reported, never merged.
TEST(roadmap_migrate_read, Inv12FoldedIdCollision) {
    const MigrationPlan plan = planFixture("identity");
    int folded = 0;
    for (const auto &it : plan.items)
        if (it.id.compare(QStringLiteral("sh-1"), Qt::CaseInsensitive) == 0)
            ++folded;
    EXPECT_EQ(folded, 2)
        << "INV-12: keying items on the folded id silently drops the second, "
           "and ANTS-3765's UNIQUE (project_id, id_fold) then fails in the "
           "half that can no longer see the source line";
    EXPECT_EQ(itemAtLine(plan, 12)->id, QStringLiteral("Sh-1"));
    EXPECT_EQ(itemAtLine(plan, 13)->id, QStringLiteral("SH-1"));
    EXPECT_EQ(noteLines(plan, "duplicate_id"), QList<int>({12, 13}))
        << "INV-12: one note per item, naming its line";
}

// --------------------------------------------------------------- INV-13 ----
// Zero ITEMS raises empty_source — a condition over the whole plan leaves the
// prose fixture silent.
TEST(roadmap_migrate_read, Inv13EmptySource) {
    const MigrationPlan empty = planFixture("empty");
    EXPECT_TRUE(empty.items.isEmpty());
    EXPECT_EQ(noteCount(empty, "empty_source"), 1);

    const MigrationPlan prose = planFixture("prose");
    EXPECT_TRUE(prose.items.isEmpty());
    EXPECT_EQ(noteCount(prose, "empty_source"), 1)
        << "INV-13: detectRoadmapFormat() answers ants-v1 for anything it does "
           "not recognise, so a detected format is no evidence of understanding";
    EXPECT_FALSE(prose.elements.isEmpty())
        << "INV-13: a prose file legitimately plans elements — which is what "
           "makes this invariant and INV-11 consistent rather than opposed";

    const MigrationPlan ok = planFixture("antsv1");
    EXPECT_EQ(noteCount(ok, "empty_source"), 0);
}

// ============================================================================
// ANTS-3766 — rotated archives as additional sources.
// Contract: docs/specs/ANTS-3766-roadmap-migration-archives.md § 3 (INV-1..13).
// Fixture roots: fixtures/archives/ (§ 6.1). INV-9 and the INV-10 refusal live
// in tests/features/roadmap_migrate_load/, which is where a store exists.
// ============================================================================

// --------------------------------------------------------- ANTS-3766 INV-1 --
TEST(roadmap_migrate_read, Ants3766Inv1DiscoveryOrderIsNumericDescending) {
    const MigrationPlan plan = planArchiveRoot("sort");
    ASSERT_EQ(plan.sources.size(), 4) << "live + three archives";
    EXPECT_EQ(sourceFileNames(plan),
              (QStringList{QStringLiteral("ROADMAP.md"), QStringLiteral("0.10.md"),
                           QStringLiteral("0.6.md"), QStringLiteral("0.5.md")}))
        << "INV-1: element 0 is always the live roadmap, then the (major, minor) "
           "INTEGER tuple DESCENDING. A lexical sort puts 0.10 LAST, since "
           "\"0.10\" < \"0.6\" as strings — the 0.10.md fixture is what makes "
           "the two orderings differ at all";
}

// --------------------------------------------------------- ANTS-3766 INV-2 --
// A root with no docs/roadmap/ plans exactly as it did before this change.
TEST(roadmap_migrate_read, Ants3766Inv2NoArchiveDirIsUnchanged) {
    const MigrationPlan plan = planArchiveRoot("noarchivedir");
    EXPECT_EQ(plan.sources.size(), 1);
    for (const auto &s : plan.sections) EXPECT_EQ(s.sourceIndex, 0);
    for (const auto &i : plan.items)    EXPECT_EQ(i.sourceIndex, 0);
    for (const auto &e : plan.elements) EXPECT_EQ(e.sourceIndex, 0);
    EXPECT_TRUE(plan.notes.isEmpty() ||
                noteCount(plan, "archive_unrecognised") == 0)
        << "INV-2: a missing archive directory is not a refusal and raises no "
           "note — it is the only silent case, and every project in the corpus "
           "but one is it";
}

// The golden list is what makes INV-2 more than self-report: a systematic
// re-slug is invisible to a field-wise dump compared only against another run
// of the same code.
TEST(roadmap_migrate_read, Ants3766Inv2SectionSlugGolden) {
    QFile f(QString::fromUtf8(ANTS_MIGRATE_SLUG_GOLDEN));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly)) << "expected-section-slugs.json";
    const QJsonObject roots =
        QJsonDocument::fromJson(f.readAll()).object()
            .value(QStringLiteral("roots")).toObject();
    ASSERT_FALSE(roots.isEmpty());

    for (auto it = roots.begin(); it != roots.end(); ++it) {
        const QString name = it.key();
        QStringList want;
        for (const auto v : it.value().toArray()) want.append(v.toString());

        QString err;
        const auto disc = RoadmapMigrate::findRoadmaps(
            fixtureDir() + QLatin1Char('/') + name, &err);
        // mixedformat/ and badutf8/ refuse the whole call by design; their live
        // slugs are still golden-listed, so skip rather than fail.
        if (!disc) continue;
        const MigrationPlan plan =
            RoadmapMigrate::planFrom(*disc, name, QStringLiteral("g"));

        QStringList got;
        for (const auto &s : plan.sections)
            if (s.sourceIndex == 0 && !s.slug.isEmpty()) got.append(s.slug);
        EXPECT_EQ(got, want)
            << "INV-2: " << name.toStdString()
            << " — the LIVE source's ordered slug list must match the golden, "
               "which tools/roadmap-slug-oracle.py derives independently of "
               "src/. Breaks when the merge path namespaces or re-numbers "
               "unconditionally rather than only for indices >= 1, which "
               "re-slugs every project's live sections and orphans its corpus";
    }
}

// --------------------------------------------------------- ANTS-3766 INV-3 --
TEST(roadmap_migrate_read, Ants3766Inv3LiveSlugsUnmovedByArchives) {
    const MigrationPlan with    = planArchiveRoot("baseline");
    const MigrationPlan without = planArchiveRoot("noarchivedir");
    ASSERT_EQ(with.sources.size(), 3);
    ASSERT_EQ(without.sources.size(), 1);
    EXPECT_EQ(slugsForSource(with, 0), slugsForSource(without, 0))
        << "INV-3: with archives present, every live-source slug is "
           "byte-identical to the slug the same root produces with "
           "docs/roadmap/ removed. Breaks when the uniquing set is shared with "
           "archives processed first, or archives are merged before the live "
           "file";
}

// --------------------------------------------------------- ANTS-3766 INV-4 --
// An archive slug is a function of that archive's filename and its own heading
// alone: appending a section to the LIVE file leaves every archive slug alone.
TEST(roadmap_migrate_read, Ants3766Inv4ArchiveSlugsSurviveALiveEdit) {
    const TempRoot t("baseline");
    const MigrationPlan before = t.plan();
    ASSERT_EQ(before.sources.size(), 3);
    const QStringList archiveBefore = slugsForSource(before, 1) + slugsForSource(before, 2);
    ASSERT_FALSE(archiveBefore.isEmpty());

    t.appendToLiveRoadmap(QStringLiteral(
        "\n### ⚡ Performance\n\n"
        "- 📋 [BASE-0007] **A fourth live perf item.**\n  Kind: perf.\n"));

    const MigrationPlan after = t.plan();
    ASSERT_EQ(after.sources.size(), 3);
    EXPECT_NE(slugsForSource(after, 0), slugsForSource(before, 0))
        << "the live edit must actually add a section, or this proves nothing";
    EXPECT_EQ(slugsForSource(after, 1) + slugsForSource(after, 2), archiveBefore)
        << "INV-4: breaks when slugs come from a counter SHARED across sources "
           "— which passes INV-1, INV-2, INV-3 and every ANTS-3757 invariant, "
           "and then shifts `performance-4` to `performance-5` on an ordinary "
           "week's edit, re-filing the archive's id-less items under a slug "
           "that no longer matches and orphaning the originals on every run";
}

// --------------------------------------------------------- ANTS-3766 INV-5 --
TEST(roadmap_migrate_read, Ants3766Inv5PartitionHoldsWithinEachSource) {
    const MigrationPlan plan = planArchiveRoot("baseline");
    ASSERT_EQ(plan.sources.size(), 3);
    for (int i = 0; i < plan.sources.size(); ++i)
        EXPECT_EQ(partitionFaultForSource(plan, i), QString())
            << "INV-5: every non-blank line of source " << i
            << " must lie inside exactly one carrier with that sourceIndex. "
               "Breaks when sourceIndex defaults to 0 on carriers built from an "
               "archive, which makes the archive's lines read as live-file "
               "lines and leaves BOTH files' partitions false while every count "
               "stays right";
}

// The legend leg. Only sources[0] may plan a legend, so an archive's own
// status-legend run is demoted to narration — which keeps every line carried.
// The real archives have no legend run, so no other root can host this.
TEST(roadmap_migrate_read, Ants3766Inv5ArchiveLegendIsDemotedNotDropped) {
    const MigrationPlan plan = planArchiveRoot("legend");
    ASSERT_EQ(plan.sources.size(), 2);
    EXPECT_FALSE(plan.legend.has_value())
        << "INV-5: MigrationPlan holds ONE optional<PlannedLegend> and the "
           "legend belongs to the project, so an archive may not contribute "
           "one; this root's live file has none";
    EXPECT_EQ(partitionFaultForSource(plan, 1), QString())
        << "INV-5: breaks when the archive's legend is DROPPED rather than "
           "demoted, which puts its lines in no carrier at all — every other "
           "fixture has no legend to lose, so this leg is the only detector";
    int narration = 0;
    for (const auto &e : plan.elements)
        if (e.sourceIndex == 1 && e.kind == QLatin1String("narration")) ++narration;
    EXPECT_GE(narration, 4) << "the four legend lines survive as narration";
}

// --------------------------------------------------------- ANTS-3766 INV-6 --
TEST(roadmap_migrate_read, Ants3766Inv6EmptySourceIsPerSource) {
    const MigrationPlan plan = planArchiveRoot("baseline");
    ASSERT_EQ(plan.sources.size(), 3);

    QList<int> raisedFor;
    for (const auto &n : plan.notes)
        if (n.code == QLatin1String("empty_source")) raisedFor.append(n.sourceIndex);
    ASSERT_EQ(raisedFor.size(), 1)
        << "INV-6: exactly one — 0.5.md is 651 bytes of prose and yields no "
           "items; the live file and 0.6.md both do";
    // Asserted on sourceIndex, NOT on detail: a human message is not
    // assertable, which is ANTS-3757 INV-1's own rule.
    const int idx = raisedFor.constFirst();
    ASSERT_GE(idx, 0);
    ASSERT_LT(idx, plan.sources.size());
    EXPECT_EQ(QFileInfo(plan.sources.at(idx).path).fileName(),
              QStringLiteral("0.5.md"))
        << "INV-6: the note's sourceIndex must RESOLVE through plan.sources to "
           "the prose-only archive. Breaks when the condition is evaluated over "
           "the MERGED plan, which raises nothing at all here (the live file "
           "has items) and so silently drops the signal that an archive parsed "
           "to nothing";
    EXPECT_NE(idx, 0) << "INV-6: none may resolve to the live file";
}

// --------------------------------------------------------- ANTS-3766 INV-7 --
TEST(roadmap_migrate_read, Ants3766Inv7ArchiveNotUtf8RefusesWholeCall) {
    QString err;
    EXPECT_FALSE(discover("badutf8", &err).has_value());
    EXPECT_EQ(err, QStringLiteral("not_utf8"))
        << "INV-7: breaks when the archive is skipped with a note instead, "
           "which commits a transaction that looks complete and is not — the "
           "load half is ONE transaction";
}

// --------------------------------------------------------- ANTS-3766 INV-8 --
TEST(roadmap_migrate_read, Ants3766Inv8UnrecognisedEntriesAreNotedNotLoaded) {
    const MigrationPlan plan = planArchiveRoot("unrecognised");
    EXPECT_EQ(plan.sources.size(), 1) << "not one of them becomes a source";

    EXPECT_EQ(noteDetails(plan, "archive_unrecognised"),
              (QStringList{QStringLiteral("0.7.0.md"), QStringLiteral("0.8.md"),
                           QStringLiteral("0.9.md"), QStringLiteral("00.07.md"),
                           QStringLiteral("README.md")}))
        << "INV-8: FIVE notes. Breaks when the filter is a *.md glob (loads "
           "0.7.0.md); when the loose ^[0-9]+\\.[0-9]+\\.md$ is used (loads "
           "00.07.md — the case withdrawn INV-12 hands to this invariant); on a "
           "silent continue; on an entryInfoList() filter that does not exclude "
           "DIRECTORIES (0.8.md is one); or on one testing isFile() ALONE, "
           "which FOLLOWS the symlink 0.9.md and loads it — the mutation a "
           "correct-looking Qt filter falls into";

    for (const auto &n : plan.notes)
        if (n.code == QLatin1String("archive_unrecognised")) {
            EXPECT_EQ(n.sourceIndex, -1)
                << "INV-8: the entry was deliberately never read, so it indexes "
                   "nothing; without the -1 it defaults to 0 and claims to be "
                   "about the live roadmap";
            EXPECT_EQ(n.line, 0) << "no file here to be inside";
        }

    // The three regular .md files each carry a uniquely-identifiable item —
    // otherwise "its items do not appear" is true of an empty file whatever
    // the code does, and that leg tests nothing.
    for (const auto &it : plan.items) {
        EXPECT_FALSE(it.id.startsWith(QStringLiteral("UNREC-PATCH")));
        EXPECT_FALSE(it.id.startsWith(QStringLiteral("UNREC-PADDED")));
        EXPECT_FALSE(it.id.startsWith(QStringLiteral("UNREC-README")));
    }
}

// The directory itself is one of those entries.
TEST(roadmap_migrate_read, Ants3766Inv8UnreadableArchiveDirIsNoted) {
    const MigrationPlan plan = planArchiveRoot("dirisfile");
    EXPECT_EQ(plan.sources.size(), 1) << "the live source is still planned";
    EXPECT_EQ(noteDetails(plan, "archive_unrecognised"),
              (QStringList{QStringLiteral("docs/roadmap")}))
        << "INV-8: a docs/roadmap that EXISTS but cannot be enumerated raises "
           "the note naming itself, while a MISSING directory raises none. "
           "Folding the two together makes the one case that loses EVERY "
           "archive at once the quietest thing this lane can do — strictly "
           "worse than a single misnamed file, which does get a note";
}

// -------------------------------------------------------- ANTS-3766 INV-10 --
TEST(roadmap_migrate_read, Ants3766Inv10SlugCollisionIsNotedNeverRenamed) {
    const MigrationPlan plan = planArchiveRoot("collision");
    ASSERT_EQ(plan.sources.size(), 2);
    EXPECT_EQ(noteCount(plan, "archive_slug_collision"), 1);

    QStringList archiveSlugs = slugsForSource(plan, 1);
    EXPECT_TRUE(archiveSlugs.contains(QStringLiteral("0-6-features")))
        << "INV-10: the archive section's slug is still 0-6-features — NOT "
           "renamed to 0-6-features-2. Renaming would shift an archive slug in "
           "response to a live-file edit, reintroducing the orphan cascade "
           "§ 2.3.1 rules out, just more rarely";
    EXPECT_TRUE(slugsForSource(plan, 0).contains(QStringLiteral("0-6-features")));
}

// -------------------------------------------------------- ANTS-3766 INV-11 --
TEST(roadmap_migrate_read, Ants3766Inv11MixedFormatRefuses) {
    QString err;
    EXPECT_FALSE(discover("mixedformat", &err).has_value());
    EXPECT_EQ(err, QStringLiteral("archive_format_mismatch"))
        << "INV-11: breaks when `format` stays on the plan and is detected from "
           "the live file alone — under which this fixture's archive parses to "
           "ZERO items with no note at all, which every other invariant here "
           "passes, because a plan that never saw those bullets cannot report "
           "them missing";
}

// The sibling leg: an archive matching its live file's format plans normally.
// Without it the fix could be "refuse every archive".
TEST(roadmap_migrate_read, Ants3766Inv11MatchingFormatPlansNormally) {
    QString err;
    const auto disc = discover("baseline", &err);
    ASSERT_TRUE(disc.has_value()) << err.toStdString();
    for (const auto &s : disc->sources)
        EXPECT_EQ(s.format, QStringLiteral("ants-v1"));
}

// Leg 3 — inheritance. A BULLET-LESS archive under a github-task-list live file
// inherits and raises nothing. The real corpus cannot catch this: its live file
// is ants-v1 and detectRoadmapFormat()'s default agrees with it by luck.
TEST(roadmap_migrate_read, Ants3766Inv11BulletlessArchiveInheritsLiveFormat) {
    QString err;
    const auto disc = discover("inherit", &err);
    ASSERT_TRUE(disc.has_value()) << err.toStdString();
    ASSERT_EQ(disc->sources.size(), 2);
    EXPECT_EQ(disc->sources.at(0).format, QStringLiteral("github-task-list"));
    EXPECT_EQ(disc->sources.at(1).format, QStringLiteral("github-task-list"))
        << "INV-11 leg 3: breaks when inheritance is omitted — the archive then "
           "reports detectRoadmapFormat()'s evidence-free ants-v1 default, "
           "mismatches, and refuses the migration of a project whose archive is "
           "merely prose";
}

// Leg 4 — the other direction, and the only detector for a reference format
// keyed on sources[0] rather than on the first EVIDENCED source.
TEST(roadmap_migrate_read, Ants3766Inv11BulletlessLiveFileInheritsArchiveFormat) {
    QString err;
    const auto disc = discover("livenosignal", &err);
    ASSERT_TRUE(disc.has_value())
        << "INV-11 leg 4: a prose-only live file beside a github-task-list "
           "archive must NOT refuse — got: " << err.toStdString();
    ASSERT_EQ(disc->sources.size(), 2);
    EXPECT_EQ(disc->sources.at(1).format, QStringLiteral("github-task-list"))
        << "the archive is the only EVIDENCED source, so it is the reference";
    EXPECT_EQ(disc->sources.at(0).format, QStringLiteral("github-task-list"))
        << "INV-11 leg 4: breaks when the reference is written as sources[0] "
           "rather than as the first evidenced source — the live file's "
           "evidence-free ants-v1 becomes the reference, the archive disagrees "
           "with it, and the whole project is refused on no evidence at all. "
           "Leg 3 passes against that mutation, since its live file is the "
           "evidenced one";
}

// -------------------------------------------------------- ANTS-3766 INV-13 --
TEST(roadmap_migrate_read, Ants3766Inv13EmptySlugTakesAnOrdinal) {
    const MigrationPlan plan = planArchiveRoot("emptyslug");
    ASSERT_EQ(plan.sources.size(), 2);
    const QStringList archiveSlugs = slugsForSource(plan, 1);

    EXPECT_TRUE(archiveSlugs.contains(QStringLiteral("0-6")))
        << "the synthetic root takes the bare <M>-<N> form, and only it may";
    bool sawOrdinal = false;
    for (const QString &s : archiveSlugs)
        if (s.startsWith(QStringLiteral("0-6-h"))) sawOrdinal = true;
    EXPECT_TRUE(sawOrdinal)
        << "INV-13: the emoji-only heading takes 0-6-h<n>. Breaks when the "
           "empty-slug case is routed through uniqueSlug() and THEN prefixed — "
           "the natural implementation, and silently wrong, because that "
           "function returns an empty base WITHOUT inserting it into `seen`, so "
           "it never uniques and every such heading collapses onto the root's "
           "slug. INV-10 cannot catch this: the collision is WITHIN one source "
           "and INV-10's note fires only archive-against-live";

    QSet<QString> distinct(archiveSlugs.begin(), archiveSlugs.end());
    EXPECT_EQ(distinct.size(), archiveSlugs.size())
        << "no two sections of one archive share a slug";
}

// The h<ordinal> substitution is ARCHIVE-ONLY: applying it at index 0 would
// change a live section's slug from "" to h<n> on any project with an
// emoji-only heading — the live-slug shift INV-4 and INV-9 forbid, and INV-3
// could not catch it because both sides of its comparison would run the new
// rule.
TEST(roadmap_migrate_read, Ants3766Inv13LivePathKeepsTodaysEmptySlugBehaviour) {
    const MigrationPlan plan = planText(QStringLiteral(
        "# Roadmap\n\n## 🎨\n\n- 📋 [X-0001] **An item.**\n  Kind: feature.\n"));
    bool sawEmpty = false;
    for (const auto &s : plan.sections)
        if (s.sourceIndex == 0 && s.level == 2 && s.slug.isEmpty()) sawEmpty = true;
    EXPECT_TRUE(sawEmpty)
        << "the live path's empty-slug hole is uniqueSlug()'s pre-existing "
           "defect; ANTS-3766 § 7 surfaces it and this change does not touch it";
}

// A bullet-less archive that DECLARES its format via the
// `<!-- ants-roadmap-format: 1 -->` marker HAS evidence, so it must mismatch a
// github-task-list live file rather than inherit from it.
//
// This is the positive case of the § 2.1.1 paragraph about the evidence
// predicate, and no other leg covers it: leg 3's archive is bullet-less AND
// undeclared, so it inherits correctly under both the right predicate and the
// wrong one. Written after implementation found the gap.
TEST(roadmap_migrate_read, Ants3766Inv11DeclaredFormatIsEvidenceNotInheritance) {
    QString err;
    EXPECT_FALSE(discover("declaredformat", &err).has_value());
    EXPECT_EQ(err, QStringLiteral("archive_format_mismatch"))
        << "INV-11: breaks when the no-signal predicate is written as \"matched "
           "no BULLET\" — the marker is matched BEFORE any bullet is examined, "
           "so a bullet-less file that explicitly declares ants-v1 would report "
           "NO evidence, and the inheritance rule would then override that "
           "declaration with the live file's github-task-list. That is the loss "
           "class § 2.1.1 exists to close, reintroduced by the exemption meant "
           "to prevent it";
}
