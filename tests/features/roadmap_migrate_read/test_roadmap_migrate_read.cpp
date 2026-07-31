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
    const auto src = RoadmapMigrate::findRoadmap(fixtureRoot(name), &err);
    EXPECT_TRUE(src.has_value()) << name << ": " << err.toStdString();
    return src ? src->markdown : QString();
}

// findRoadmap + planFrom over a committed fixture, as migration itself runs.
MigrationPlan planFixture(const char *name) {
    QString err;
    const auto src = RoadmapMigrate::findRoadmap(fixtureRoot(name), &err);
    EXPECT_TRUE(src.has_value()) << name << ": " << err.toStdString();
    if (!src) return {};
    return RoadmapMigrate::planFrom(src->markdown, src->path,
                                    QString::fromUtf8(name),
                                    QString::fromUtf8(name));
}

MigrationPlan planText(const QString &markdown) {
    return RoadmapMigrate::planFrom(markdown, QStringLiteral("<inline>"),
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
    QString d = p.projectName + '|' + p.exportSlug + '|' + p.sourcePath + '|' +
                p.format + '\n';
    for (const auto &s : p.sections)
        d += QStringLiteral("S %1|%2|%3|%4|%5|%6-%7\n")
                 .arg(s.slug, s.title, s.intro, QString::number(s.level),
                      s.parentSlug, QString::number(s.firstLine))
                 .arg(s.lastLine);
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
                 .arg(i.lastLine);
    for (const auto &e : p.elements)
        d += QStringLiteral("E %1|%2|%3|%4|%5-%6\n")
                 .arg(e.kind, e.payload, e.sectionSlug,
                      QString::number(e.position), QString::number(e.firstLine))
                 .arg(e.lastLine);
    if (p.legend)
        d += QStringLiteral("L %1|%2-%3\n")
                 .arg(QString::fromUtf8(QJsonDocument(p.legend->entries).toJson(
                          QJsonDocument::Compact)))
                 .arg(p.legend->firstLine)
                 .arg(p.legend->lastLine);
    for (const auto &n : p.notes)
        d += QStringLiteral("N %1|%2|%3\n").arg(n.code, n.detail).arg(n.line);
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

}  // namespace

// ---------------------------------------------------------------- INV-1 ----
// Discovery per § 2.2: case-insensitive, non-recursive, three refusals
// asserted on their code rather than on a human message.
TEST(roadmap_migrate_read, Inv1Discovery) {
    QString err;
    const auto upper = RoadmapMigrate::findRoadmap(fixtureRoot("discovery/upper"), &err);
    ASSERT_TRUE(upper.has_value()) << err.toStdString();
    EXPECT_TRUE(upper->markdown.contains(QStringLiteral("UP-0001")))
        << "INV-1: the file's own text is returned, not just its path";
    EXPECT_TRUE(upper->path.endsWith(QStringLiteral("ROADMAP.md")));

    err.clear();
    const auto lower = RoadmapMigrate::findRoadmap(fixtureRoot("discovery/lower"), &err);
    ASSERT_TRUE(lower.has_value()) << err.toStdString();
    EXPECT_TRUE(lower->markdown.contains(QStringLiteral("LOW-0001")))
        << "INV-1: an uppercase-only glob drops the one project that owns the "
           "whole pass corpus";
    EXPECT_TRUE(lower->path.endsWith(QStringLiteral("roadmap.md")));

    err.clear();
    EXPECT_FALSE(RoadmapMigrate::findRoadmap(fixtureRoot("discovery/none"), &err));
    EXPECT_EQ(err, QStringLiteral("not_found"));

    err.clear();
    EXPECT_FALSE(RoadmapMigrate::findRoadmap(fixtureRoot("discovery/both"), &err));
    EXPECT_EQ(err, QStringLiteral("case_ambiguous"))
        << "INV-1: either choice silently discards a whole project's roadmap";

    err.clear();
    EXPECT_FALSE(RoadmapMigrate::findRoadmap(fixtureRoot("discovery/badutf8"), &err));
    EXPECT_EQ(err, QStringLiteral("not_utf8"))
        << "INV-1: a lossy U+FFFD decode leaves INV-11 green over corrupted "
           "content";

    err.clear();
    EXPECT_FALSE(RoadmapMigrate::findRoadmap(fixtureRoot("discovery/absent"), &err));
    EXPECT_EQ(err, QStringLiteral("not_found"));
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
    const PlannedItem *closedNoId = itemAtLine(plan, 14);
    ASSERT_NE(closedNoId, nullptr);
    EXPECT_FALSE(itemAtLine(plan, 9)->closed);
    EXPECT_FALSE(itemAtLine(plan, 11)->closed);
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
    for (const QString &word :
         {QStringLiteral("partial"), QStringLiteral("un-gated")}) {
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
                                QStringLiteral("deferred")}) {
        const MigrationPlan p = planText(passDoc(word));
        EXPECT_EQ(prov(p.items[0], "status"), QStringLiteral("asserted"))
            << "INV-6: '" << word.toStdString()
            << "' — only the notation differs from an author writing an emoji";
        EXPECT_EQ(noteCount(p, "status_defaulted"), 0);
    }

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

    const MigrationPlan first =
        RoadmapMigrate::planFrom(markdown, QStringLiteral("/x/ROADMAP.md"),
                                 QStringLiteral("P"), QStringLiteral("p"));
    const MigrationPlan second =
        RoadmapMigrate::planFrom(markdown, QStringLiteral("/x/ROADMAP.md"),
                                 QStringLiteral("P"), QStringLiteral("p"));
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
    EXPECT_EQ(plan.format, QStringLiteral("pass-headings"));

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
    EXPECT_EQ(plan.items[0].status, QStringLiteral("planned"))
        << "INV-10: the first classifying line wins; the last would make this "
           "shipped";
    EXPECT_EQ(extra(plan.items[0], "source_status"),
              QStringLiteral("partial (v3.6.20). Phase A landed"));
    EXPECT_EQ(plan.items[1].status, QStringLiteral("considered"));

    EXPECT_EQ(noteLines(plan, "orphan_status_line"), QList<int>({3}))
        << "INV-10: a Status line belonging to no pass block is reported, not "
           "imported and not dropped";
    EXPECT_EQ(noteCount(plan, "status_defaulted"), 2);
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
