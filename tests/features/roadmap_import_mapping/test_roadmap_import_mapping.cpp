// Feature-conformance test for ANTS-4065 — the markdown→store import mapping.
// Contract: docs/specs/ANTS-4065-import-mapping-contract.md § 2 (the surface)
// and § 3 (INV-1..11). Fixture and must-fail-first notes:
// tests/features/roadmap_import_mapping/spec.md.
//
// Behavioural throughout — the spec carries no source-grep invariant. The
// documents are inline because every case is a few-line roadmap and the text
// under test is more useful beside its assertion; the two cases needing a
// project on disk write theirs into a QTemporaryDir.
//
// RoadmapStore is constructed with an EXPLICIT path in every case. The default
// resolves under XDG_DATA_HOME, which is the developer's live store.

#include <gtest/gtest.h>

#include "roadmapmigrate.h"
#include "roadmapmigrateload.h"
#include "roadmapmigrateverb.h"
#include "roadmapparse.h"
#include "roadmaprender.h"
#include "roadmapstore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <memory>

using RoadmapMigrate::MigrationPlan;
using RoadmapMigrate::Note;
using RoadmapMigrate::PlannedItem;

namespace {

MigrationPlan planText(const QString &markdown) {
    RoadmapMigrate::Source s;
    s.path     = QStringLiteral("<inline>");
    s.markdown = markdown;
    s.format   = RoadmapParse::detectRoadmapFormat(markdown.split(QLatin1Char('\n')));
    RoadmapMigrate::Discovery d;
    d.sources.append(s);
    return RoadmapMigrate::planFrom(d, QStringLiteral("Demo"), QStringLiteral("demo"));
}

// A one-bullet ants-v1 document. The format marker and a heading are what make
// detectRoadmapFormat() classify it as ants-v1 rather than guessing.
QString doc(const QString &bullets) {
    return QStringLiteral("<!-- ants-roadmap-format: 1 -->\n"
                          "\n"
                          "# Demo — Roadmap\n"
                          "\n"
                          "## Now\n"
                          "\n") + bullets;
}

const PlannedItem *itemById(const MigrationPlan &plan, const char *id) {
    for (const PlannedItem &it : plan.items)
        if (it.id == QLatin1String(id))
            return &it;
    return nullptr;
}

QString provOf(const PlannedItem &it, const char *field) {
    return it.provenance.value(QLatin1String(field)).toString();
}

QString extraOf(const PlannedItem &it, const char *key) {
    return it.extras.value(QLatin1String(key)).toString();
}

bool hasNote(const QVector<Note> &notes, const char *code, const QString &detailNeedle) {
    for (const Note &n : notes)
        if (n.code == QLatin1String(code) && n.detail.contains(detailNeedle))
            return true;
    return false;
}

QString notesDump(const QVector<Note> &notes) {
    QStringList out;
    for (const Note &n : notes)
        out.append(n.code + QStringLiteral("(") + n.detail + QStringLiteral(")"));
    return out.join(QStringLiteral(", "));
}

bool writeFile(const QString &path, const QString &text) {
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return f.write(text.toUtf8()) >= 0;
}

// A store on an explicit path, on the Bulk connection a migration load requires.
std::unique_ptr<RoadmapStore> openStore(const QString &path, QString *err) {
    auto s = std::make_unique<RoadmapStore>(path, RoadmapStore::kDefaultHistoryCapBytes,
                                            RoadmapStore::Access::Bulk);
    if (!s->open(err))
        return nullptr;
    return s;
}

}  // namespace

// ---------------------------------------------------------------- INV-1 ---
// No import defaults a field without emitting a note naming that field.

TEST(RoadmapImportMapping, DefaultedFieldsAreNoted) {
    const auto plan = planText(doc(QStringLiteral(
        "- 📋 [DEMO-0001] **A bullet that declares neither kind nor source.**\n"
        "  Layman: A thing.\n")));

    const PlannedItem *it = itemById(plan, "DEMO-0001");
    ASSERT_NE(it, nullptr);
    EXPECT_EQ(it->kind, QStringLiteral("implement"));
    EXPECT_EQ(provOf(*it, "kind"), QStringLiteral("defaulted"));
    EXPECT_EQ(it->source, QStringLiteral("planned"));
    EXPECT_EQ(provOf(*it, "source"), QStringLiteral("defaulted"));

    EXPECT_TRUE(hasNote(plan.notes, "field_defaulted", QStringLiteral("kind")))
        << "no field_defaulted note names `kind`: " << notesDump(plan.notes).toStdString();
    EXPECT_TRUE(hasNote(plan.notes, "field_defaulted", QStringLiteral("source")))
        << "no field_defaulted note names `source`: " << notesDump(plan.notes).toStdString();
}

// A field the bullet DECLARES raises no note — otherwise the tally the note
// feeds counts the whole corpus and says nothing.
TEST(RoadmapImportMapping, DeclaredFieldsAreNotNoted) {
    const auto plan = planText(doc(QStringLiteral(
        "- 📋 [DEMO-0002] **A bullet that declares both.**\n"
        "  Layman: A thing.\n"
        "  Kind: security.\n"
        "  Source: user-2026-08-09.\n")));

    const PlannedItem *it = itemById(plan, "DEMO-0002");
    ASSERT_NE(it, nullptr);
    EXPECT_EQ(provOf(*it, "kind"), QStringLiteral("asserted"));
    EXPECT_EQ(provOf(*it, "source"), QStringLiteral("asserted"));
    EXPECT_FALSE(hasNote(plan.notes, "field_defaulted", QString()))
        << notesDump(plan.notes).toStdString();
}

// ---------------------------------------------------------------- INV-2 ---
// A bullet declaring `Kind:` inline, not at line start, imports with that kind.

TEST(RoadmapImportMapping, InlineKindDeclarationIsRead) {
    const auto plan = planText(doc(QStringLiteral(
        "- 📋 [DEMO-0003] **An inline declaration.**\n"
        "  Body text. Kind: security.\n"
        "  Layman: A thing.\n")));

    const PlannedItem *it = itemById(plan, "DEMO-0003");
    ASSERT_NE(it, nullptr);
    EXPECT_EQ(it->kind, QStringLiteral("security"));
    EXPECT_EQ(provOf(*it, "kind"), QStringLiteral("asserted"));
}

// ---------------------------------------------------------------- INV-3 ---
// A bullet QUOTING the label does not declare it (ANTS-3722's guard).

TEST(RoadmapImportMapping, BacktickedLabelIsNotADeclaration) {
    const auto plan = planText(doc(QStringLiteral(
        "- 📋 [DEMO-0004] **A bullet about the format.**\n"
        "  This one discusses the `Kind:` trailer rather than declaring one.\n"
        "  Layman: A thing.\n")));

    const PlannedItem *it = itemById(plan, "DEMO-0004");
    ASSERT_NE(it, nullptr);
    EXPECT_EQ(it->kind, QStringLiteral("implement"));
    EXPECT_EQ(provOf(*it, "kind"), QStringLiteral("defaulted"));
    EXPECT_TRUE(extraOf(*it, "source_kind").isEmpty())
        << "a quoted label was captured as a value: "
        << extraOf(*it, "source_kind").toStdString();
}

// --------------------------------------------------- INV-2 (bold form) ---
// ANTS-4077 — the label may be written bold, exactly as `Layman:` may be
// (ANTS-1861, because roadmap_log writes that version). 20 bullets in this
// project declare `**Kind:**` and every one of them was read as declaring
// nothing; § 2.2's un-anchoring alone made it worse, capturing the closing `**`
// and the prose after it into extras.source_kind.

TEST(RoadmapImportMapping, BoldKindLabelIsADeclaration) {
    const auto plan = planText(doc(QStringLiteral(
        "- 📋 [DEMO-0090] **A bold-label declaration.**\n"
        "  Layman: A thing.\n"
        "  **Kind:** refactor.\n")));

    const PlannedItem *it = itemById(plan, "DEMO-0090");
    ASSERT_NE(it, nullptr);
    EXPECT_EQ(it->kind, QStringLiteral("refactor"));
    EXPECT_EQ(provOf(*it, "kind"), QStringLiteral("asserted"));
    EXPECT_TRUE(extraOf(*it, "source_kind").isEmpty())
        << "a canonical value needs no source_kind: "
        << extraOf(*it, "source_kind").toStdString();
}

// The bold form inline, which is how the corpus actually writes it — beside a
// bold `**Lanes:**` on the same line.
TEST(RoadmapImportMapping, BoldKindLabelInlineIsADeclaration) {
    const auto plan = planText(doc(QStringLiteral(
        "- 📋 [DEMO-0091] **A bold trailer written inline.**\n"
        "  Layman: A thing.\n"
        "  **Kind:** refactor. **Lanes:** core.\n")));

    const PlannedItem *it = itemById(plan, "DEMO-0091");
    ASSERT_NE(it, nullptr);
    EXPECT_EQ(it->kind, QStringLiteral("refactor"));
    EXPECT_EQ(it->lanes, (QStringList{QStringLiteral("core")}))
        << "the bold Lanes: on the same line must still parse";
}

// The contract is PARITY with the plain spelling, not a new rule for the bold
// one. A qualifier-bearing value runs to the first period in both — so
// `refactor (no behaviour change)` is one unrecognised value, defaulted with the
// original preserved, exactly as the plain form has always behaved. Asserting
// `refactor` here instead would be inventing a qualifier-stripping rule that
// exists for no other label; the qualifier is § 2.1's problem, not this one's.
TEST(RoadmapImportMapping, BoldAndPlainKindLabelsAgree) {
    const auto bold = planText(doc(QStringLiteral(
        "- 📋 [DEMO-0093] **Bold spelling.**\n"
        "  Layman: A thing.\n"
        "  **Kind:** refactor (no behaviour change).\n")));
    const auto plain = planText(doc(QStringLiteral(
        "- 📋 [DEMO-0093] **Plain spelling.**\n"
        "  Layman: A thing.\n"
        "  Kind: refactor (no behaviour change).\n")));

    const PlannedItem *b = itemById(bold, "DEMO-0093");
    const PlannedItem *p = itemById(plain, "DEMO-0093");
    ASSERT_NE(b, nullptr);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(b->kind, p->kind);
    EXPECT_EQ(extraOf(*b, "source_kind"), extraOf(*p, "source_kind"));
    EXPECT_EQ(extraOf(*b, "source_kind"),
              QStringLiteral("refactor (no behaviour change)"))
        << "the declared text must survive verbatim whichever spelling wrote it";
}

// INV-3's guard has to cover the bold form too: `` `**Kind:**` `` is a
// quotation, and the plain backtick lookbehind cannot see past the asterisks.
TEST(RoadmapImportMapping, BacktickedBoldLabelIsNotADeclaration) {
    const auto plan = planText(doc(QStringLiteral(
        "- 📋 [DEMO-0092] **A bullet about the format.**\n"
        "  The writer emits `**Kind:** implement.` on every bullet.\n"
        "  Layman: A thing.\n")));

    const PlannedItem *it = itemById(plan, "DEMO-0092");
    ASSERT_NE(it, nullptr);
    EXPECT_EQ(provOf(*it, "kind"), QStringLiteral("defaulted"))
        << "a quoted bold label was read as a declaration";
}

// ---------------------------------------------------------------- INV-4 ---
// Every non-identity `kind` mapping preserves the original in
// extras.source_kind. The four rows below the divider are § 2.1's additions.

TEST(RoadmapImportMapping, MappedKindPreservesTheOriginal) {
    struct Case { const char *id; const char *declared; const char *mapped; };
    static const Case kCases[] = {
        // roadmap-data-model.md § 7.4's existing entries — a sample, not the set.
        {"DEMO-0010", "bugfix",           "fix"},
        {"DEMO-0011", "docs",             "doc"},
        // § 2.1's four mechanical additions.
        {"DEMO-0012", "bug",              "fix"},
        {"DEMO-0013", "performance",      "perf"},
        {"DEMO-0014", "process + tooling", "chore"},
        {"DEMO-0015", "audit",            "audit-fix"},
    };

    QString bullets;
    for (const Case &c : kCases) {
        bullets += QStringLiteral("- 📋 [%1] **A mapped kind.**\n"
                                  "  Layman: A thing.\n"
                                  "  Kind: %2.\n"
                                  "  Source: test.\n")
                       .arg(QLatin1String(c.id), QLatin1String(c.declared));
    }
    const auto plan = planText(doc(bullets));

    for (const Case &c : kCases) {
        const PlannedItem *it = itemById(plan, c.id);
        ASSERT_NE(it, nullptr) << c.declared;
        EXPECT_EQ(it->kind, QLatin1String(c.mapped)) << c.declared;
        EXPECT_EQ(provOf(*it, "kind"), QStringLiteral("asserted")) << c.declared;
        EXPECT_EQ(extraOf(*it, "source_kind"), QLatin1String(c.declared))
            << "the mapping is irreversible: " << c.declared;
    }
}

// ---------------------------------------------------------------- INV-5 ---
// A `source` marked provenance = defaulted does not render a `Source:` line;
// layman, lanes and evidence render exactly as they do today.

namespace {

RoadmapStore::ItemWrite renderableItem() {
    RoadmapStore::ItemWrite it;
    it.id       = QStringLiteral("DEMO-0020");
    it.status   = QStringLiteral("planned");
    it.headline = QStringLiteral("An item with every trailer.");
    it.kind     = QStringLiteral("implement");
    it.source   = QStringLiteral("planned");
    it.layman   = QStringLiteral("A thing");
    it.lanes    = {QStringLiteral("core"), QStringLiteral("tests")};
    it.evidence = {QStringLiteral("docs/a.md")};
    return it;
}

}  // namespace

TEST(RoadmapImportMapping, DefaultedSourceIsNotRendered) {
    RoadmapStore::ItemWrite it = renderableItem();
    it.provenance.insert(QStringLiteral("source"), QStringLiteral("defaulted"));

    const QString text = RoadmapRender::bulletText(it);
    EXPECT_FALSE(text.contains(QStringLiteral("Source:")))
        << "a defaulted source was written back into the file:\n" << text.toStdString();
    // The three trailers that never carry provenance at all must be untouched —
    // the failure mode § 2.4 exists to prevent is a rule phrased over absent
    // provenance, which would suppress all three.
    EXPECT_TRUE(text.contains(QStringLiteral("**Layman:** A thing"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("Lanes: core, tests"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("Evidence: docs/a.md"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("Kind: implement."))) << text.toStdString();
}

// The direction of § 2.4's test is the whole finding: ABSENT provenance means
// "not a defaultable field", never "defaulted". A row written by roadmap_log
// carries no `source` key and must keep its Source: line.
TEST(RoadmapImportMapping, AbsentProvenanceStillRendersSource) {
    const RoadmapStore::ItemWrite it = renderableItem();   // provenance empty
    const QString text = RoadmapRender::bulletText(it);
    EXPECT_TRUE(text.contains(QStringLiteral("Source: planned.")))
        << "a row with no provenance lost its Source: line:\n" << text.toStdString();
}

TEST(RoadmapImportMapping, AssertedSourceIsRendered) {
    RoadmapStore::ItemWrite it = renderableItem();
    it.source = QStringLiteral("user-2026-08-09");
    it.provenance.insert(QStringLiteral("source"), QStringLiteral("asserted"));
    const QString text = RoadmapRender::bulletText(it);
    EXPECT_TRUE(text.contains(QStringLiteral("Source: user-2026-08-09.")))
        << text.toStdString();
}

// ---------------------------------------------------------------- INV-7 ---
// A Source:/Evidence: value naming a path that does not exist imports
// successfully, with a note and extras.unresolved_path.

TEST(RoadmapImportMapping, UnresolvedPathsAreNotedNotRefused) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = dir.filePath(QStringLiteral("proj"));
    ASSERT_TRUE(writeFile(root + QStringLiteral("/docs/here.md"), QStringLiteral("x\n")));
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"), doc(QStringLiteral(
        "- 📋 [DEMO-0030] **An item citing a file that has moved.**\n"
        "  Layman: A thing.\n"
        "  Kind: implement.\n"
        "  Source: docs/gone.md.\n"
        "  Evidence: docs/here.md, docs/also-gone.log\n"
        "- 📋 [DEMO-0031] **An item whose source is a recognised form, not a path.**\n"
        "  Layman: A thing.\n"
        "  Kind: implement.\n"
        "  Source: user-2026-08-09.\n"))));

    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    ASSERT_TRUE(disc) << err.toStdString();
    MigrationPlan plan = RoadmapMigrate::planFrom(*disc, QStringLiteral("Demo"),
                                                  QStringLiteral("demo"));
    RoadmapMigrate::validatePaths(plan, root);

    const PlannedItem *cited = itemById(plan, "DEMO-0030");
    ASSERT_NE(cited, nullptr) << "a bullet citing a missing file was refused";
    const QJsonArray unresolved =
        cited->extras.value(QStringLiteral("unresolved_path")).toArray();
    QStringList got;
    for (const QJsonValue &v : unresolved)
        got.append(v.toString());
    got.sort();
    EXPECT_EQ(got, (QStringList{QStringLiteral("docs/also-gone.log"),
                                QStringLiteral("docs/gone.md")}))
        << "one item can lose more than one path, so this is an array: "
        << got.join(QStringLiteral(", ")).toStdString();
    EXPECT_TRUE(hasNote(plan.notes, "unresolved_path", QStringLiteral("docs/gone.md")))
        << notesDump(plan.notes).toStdString();

    // § 3.5.3's own Source: vocabulary is full of hyphenated tokens that must
    // not be mistaken for filenames.
    const PlannedItem *plain = itemById(plan, "DEMO-0031");
    ASSERT_NE(plain, nullptr);
    EXPECT_FALSE(plain->extras.contains(QStringLiteral("unresolved_path")))
        << "a recognised source form was treated as a path";
}

// ------------------------------------------------------ § 2.3's tally ---
// The run-level `defaulted_fields` report. Not an invariant of its own, but
// § 2.3 makes it normative and it is the figure a migration is READ from — the
// notes array is capped (§ 4), so a reader counting `field_defaulted` entries
// in it would under-report exactly the run that most needed reporting.

TEST(RoadmapImportMapping, DefaultedFieldsTallyIsPerFieldAndComplete) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = dir.filePath(QStringLiteral("proj"));
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"), doc(QStringLiteral(
        // Declares neither — one `kind`, one `source`.
        "- 📋 [DEMO-0080] **Declares neither.**\n"
        "  Layman: A thing.\n"
        // Declares a kind nothing recognises — still a defaulted `kind`, per
        // § 2.3's \"the note fires on the default itself rather than on whether
        // anything survived it\", and a second defaulted `source`.
        "- 📋 [DEMO-0081] **Declares a kind nothing recognises.**\n"
        "  Layman: A thing.\n"
        "  Kind: wibble.\n"
        // Declares both — contributes to neither count.
        "- 📋 [DEMO-0082] **Declares both.**\n"
        "  Layman: A thing.\n"
        "  Kind: doc.\n"
        "  Source: test.\n"))));

    RoadmapMigrateVerb::Request req;
    req.projectRoot = QFileInfo(root).canonicalFilePath();
    req.projectName = QStringLiteral("Demo");
    req.exportSlug  = QStringLiteral("demo");
    req.changedAt   = QStringLiteral("2026-08-09T10:00:00Z");

    const QJsonObject env =
        RoadmapMigrateVerb::run(dir.filePath(QStringLiteral("tally.db")), req);
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(env).toJson(QJsonDocument::Compact).toStdString();

    const QJsonObject tally =
        env.value(QStringLiteral("defaulted_fields")).toObject();
    EXPECT_EQ(tally.value(QStringLiteral("kind")).toInt(), 2)
        << "the unmapped-value default was not counted";
    EXPECT_EQ(tally.value(QStringLiteral("source")).toInt(), 2);
    EXPECT_FALSE(tally.contains(QStringLiteral("status")))
        << "no bullet here defaulted its status";
}

// ---------------------------------------------------------------- INV-8 ---
// The emoji→status mapping is total and closed over the four documented
// markers, so no input can reach the fifth.

TEST(RoadmapImportMapping, StatusMappingIsTotalOverTheFourMarkers) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = dir.filePath(QStringLiteral("proj"));
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"), doc(QStringLiteral(
        "- 📋 [DEMO-0040] **Planned.**\n"
        "  Layman: A thing.\n"
        "  Kind: implement.\n"
        "  Source: test.\n"
        "- 🚧 [DEMO-0041] **In progress.**\n"
        "  Layman: A thing.\n"
        "  Kind: implement.\n"
        "  Source: test.\n"
        "- ✅ [DEMO-0042] **Shipped.**\n"
        "  Kind: implement.\n"
        "  Source: test.\n"
        "- 💭 [DEMO-0043] **Considered.**\n"
        "  Layman: A thing.\n"
        "  Kind: implement.\n"
        "  Source: test.\n"
        "- ❓ [DEMO-0044] **A marker no legend documents.**\n"
        "  Layman: A thing.\n"
        "  Kind: implement.\n"
        "  Source: test.\n"))));

    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    ASSERT_TRUE(disc) << err.toStdString();
    const auto plan = RoadmapMigrate::planFrom(*disc, QStringLiteral("Demo"),
                                               QStringLiteral("demo"));

    struct Row { const char *id; const char *status; };
    static const Row kRows[] = {
        {"DEMO-0040", "planned"},     {"DEMO-0041", "in-progress"},
        {"DEMO-0042", "shipped"},     {"DEMO-0043", "considered"},
    };
    for (const Row &r : kRows) {
        const PlannedItem *it = itemById(plan, r.id);
        ASSERT_NE(it, nullptr) << r.id;
        EXPECT_EQ(it->status, QLatin1String(r.status)) << r.id;
    }

    // The undocumented marker, and this is where the spec's INV-8 test clause
    // is wrong about the code. It says the malformed bullet "defaults to
    // `planned` with a note". Measured 2026-08-09: it becomes no item at all —
    // RoadmapParse::stripInlineEmoji() recognises exactly the four markers, and
    // a `- ` line carrying anything else is not a bullet, so it never reaches
    // makeItem(). Making it an item instead would mean every unmarked `- ` line
    // in an ants-v1 document became one, which is a change to ANTS-3757's
    // bullet grammar and a far larger loss than the one INV-8 guards.
    //
    // Nothing is dropped: § 2.11's partition carries the line as narration, so
    // the assertion below is the honest form of INV-8's second clause — the
    // mapping is total over the four markers and the fifth status is
    // unreachable, with no line lost on the way. Tracked as ANTS-4076.
    EXPECT_EQ(itemById(plan, "DEMO-0044"), nullptr)
        << "an undocumented marker was admitted as an item";
    bool carried = false;
    for (const RoadmapMigrate::PlannedElement &e : plan.elements)
        if (e.payload.contains(QStringLiteral("DEMO-0044")))
            carried = true;
    EXPECT_TRUE(carried) << "the undocumented-marker line was dropped outright";
    for (const PlannedItem &it : plan.items)
        EXPECT_NE(it.status, QStringLiteral("dropped"))
            << "a row the render cannot express: " << it.id.toStdString();

    auto store = openStore(dir.filePath(QStringLiteral("store.db")), &err);
    ASSERT_TRUE(store) << err.toStdString();
    RoadmapMigrateLoad::Options opts;
    opts.changedAt   = QStringLiteral("2026-08-09T10:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(*store, plan, opts);
    ASSERT_TRUE(out.ok) << out.error.toStdString();
    const auto stored = store->readItems(out.projectId, &err);
    ASSERT_TRUE(stored) << err.toStdString();
    for (const RoadmapStore::ItemWrite &w : *stored)
        EXPECT_NE(w.status, QStringLiteral("dropped")) << w.id.toStdString();
}

// ---------------------------------------------------------------- INV-9 ---
// A lowercase `kind:` label does not parse as a declaration — § 2.2's
// deliberate ANTS-3407 reversal, tested rather than assumed.

TEST(RoadmapImportMapping, LowercaseKindLabelIsNotADeclaration) {
    const auto plan = planText(doc(QStringLiteral(
        "- 📋 [DEMO-0050] **A hand-typed lowercase label.**\n"
        "  Body. kind: security.\n"
        "  Layman: A thing.\n")));

    const PlannedItem *it = itemById(plan, "DEMO-0050");
    ASSERT_NE(it, nullptr);
    EXPECT_EQ(it->kind, QStringLiteral("implement"));
    EXPECT_EQ(provOf(*it, "kind"), QStringLiteral("defaulted"));
    EXPECT_TRUE(hasNote(plan.notes, "field_defaulted", QStringLiteral("kind")))
        << notesDump(plan.notes).toStdString();
}

// --------------------------------------------------------------- INV-10 ---
// Un-anchoring changes render suppression ONLY where a mid-prose `Kind:` value
// equals the column's. shadows() is value equality, not presence.

TEST(RoadmapImportMapping, UnAnchoringChangesSuppressionOnlyOnValueEquality) {
    RoadmapStore::ItemWrite base = renderableItem();
    base.kind = QStringLiteral("refactor");

    // (a) body says nothing about the label — the required trailer is emitted.
    RoadmapStore::ItemWrite noMention = base;
    noMention.body = QStringLiteral("Plain body prose.");
    EXPECT_TRUE(RoadmapRender::bulletText(noMention)
                    .contains(QStringLiteral("Kind: refactor.")))
        << RoadmapRender::bulletText(noMention).toStdString();

    // (b) mid-prose value DIFFERS from the column — still emitted, because
    // suppressing on presence would leave the stale value as the only one.
    RoadmapStore::ItemWrite differs = base;
    differs.body = QStringLiteral("This used to be Kind: security. Now it is not.");
    EXPECT_TRUE(RoadmapRender::bulletText(differs)
                    .contains(QStringLiteral("Kind: refactor.")))
        << RoadmapRender::bulletText(differs).toStdString();

    // (c) mid-prose value EQUALS the column — suppressed, so the bullet does
    // not carry the same statement twice. This is the case the anchor hid.
    RoadmapStore::ItemWrite equals = base;
    equals.body = QStringLiteral("Body text. Kind: refactor.");
    const QString text = RoadmapRender::bulletText(equals);
    EXPECT_EQ(text.count(QStringLiteral("Kind: refactor.")), 1)
        << "the body's own declaration was duplicated by the trailer:\n"
        << text.toStdString();
}

// --------------------------------------------------------------- INV-11 ---
// When a bullet contains more than one `Kind:` match, the parser takes the
// LAST — the occurrence the render authored.

TEST(RoadmapImportMapping, LastKindMatchWins) {
    const auto plan = planText(doc(QStringLiteral(
        "- 📋 [DEMO-0060] **A bullet whose body mentions an older kind.**\n"
        "  This was filed as Kind: refactor. It is not that any more.\n"
        "  Layman: A thing.\n"
        "  Kind: security.\n")));

    const PlannedItem *it = itemById(plan, "DEMO-0060");
    ASSERT_NE(it, nullptr);
    EXPECT_EQ(it->kind, QStringLiteral("security"))
        << "the stale prose value won over the trailer the render authored";
}

// ---------------------------------------------------------------- INV-6 ---
// import(render(store)) changes none of § 2.6's nine governed columns.
//
// One item per interesting shape (§ 6).
//
// The gate is the LAST of three cycles, and that is § 2.6's own wording — "for
// a MIGRATED project". The first render legitimately materialises what the
// source omitted: `Kind:` is required by roadmap-format.md § 3.5, so a bullet
// that declared none gains the line, and `body` (which holds the bullet's
// continuation lines verbatim) grows by exactly that line when the file is read
// back. Asserting identity across that would be asserting that a hand-written
// roadmap is already in rendered form, which no migration input is.
//
// Three cycles and not two, because the settling appears between the FIRST and
// SECOND stores — cycle 2 is the first import that reads a rendered file — so
// two cycles give the gate nothing to compare. Measured 2026-08-09: 2 of 5
// fixture items settle (`body` only), and nothing moves after that. This is also
// the plan's D4 rule, that a single pass cannot distinguish "stable" from
// "drifting slowly", made into the test's shape.
//
// The settling cycle is asserted too, just not for identity: every governed
// column except `body` must already be stable there, so real drift cannot hide
// behind it. The per-column diff is what Phase D4 reads.

namespace {

constexpr const char *kRoundTripDoc =
    "<!-- ants-roadmap-format: 1 -->\n"
    "\n"
    "# Demo — Roadmap\n"
    "\n"
    "## Now\n"
    "\n"
    "- 📋 [DEMO-0070] **An inline trailer.**\n"
    "  Body text. Kind: security.\n"
    "  Layman: A thing.\n"
    "  Source: test.\n"
    "- 📋 [DEMO-0071] **A quoted label.**\n"
    "  This discusses the `Kind:` trailer.\n"
    "  Layman: A thing.\n"
    "  Kind: implement.\n"
    "  Source: test.\n"
    "- 🚧 [DEMO-0072] **An absent kind.**\n"
    "  Layman: A thing.\n"
    "  Source: test.\n"
    "- ✅ [DEMO-0073] **A mapped kind.**\n"
    "  Kind: bugfix.\n"
    "  Source: test.\n"
    "- 📋 [DEMO-0074] **An unresolved path.**\n"
    "  Layman: A thing.\n"
    "  Kind: doc.\n"
    "  Source: docs/gone.md.\n"
    "  Lanes: core, tests.\n";

// § 2.6's governed set, minus `id` (the match key, which cannot change on a
// matched row) and minus the columns it excludes.
QStringList governedDrift(const RoadmapStore::ItemWrite &a,
                          const RoadmapStore::ItemWrite &b) {
    QStringList out;
    const auto cmp = [&out](const char *col, const QString &x, const QString &y) {
        if (x != y) out.append(QLatin1String(col));
    };
    cmp("status",   a.status,   b.status);
    cmp("headline", a.headline, b.headline);
    cmp("kind",     a.kind,     b.kind);
    cmp("source",   a.source,   b.source);
    cmp("layman",   a.layman,   b.layman);
    cmp("body",     a.body,     b.body);
    cmp("lanes",    a.lanes.join(QLatin1Char('\x1f')),
                    b.lanes.join(QLatin1Char('\x1f')));
    cmp("evidence", a.evidence.join(QLatin1Char('\x1f')),
                    b.evidence.join(QLatin1Char('\x1f')));
    return out;
}

QHash<QString, RoadmapStore::ItemWrite> byId(const QHash<qint64, RoadmapStore::ItemWrite> &in) {
    QHash<QString, RoadmapStore::ItemWrite> out;
    for (auto i = in.constBegin(); i != in.constEnd(); ++i)
        out.insert(i.value().id, i.value());
    return out;
}

}  // namespace

TEST(RoadmapImportMapping, RenderThenImportIsIdentityOverGovernedColumns) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = dir.filePath(QStringLiteral("proj"));
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"),
                          QString::fromUtf8(kRoundTripDoc)));

    QString err;
    auto store = openStore(dir.filePath(QStringLiteral("store.db")), &err);
    ASSERT_TRUE(store) << err.toStdString();

    RoadmapMigrateLoad::Options opts;
    opts.changedAt   = QStringLiteral("2026-08-09T10:00:00Z");
    opts.projectRoot = root;

    // One import of whatever is on disk, then one render back over it.
    const auto cycle = [&](RoadmapMigrateLoad::Outcome *out) {
        const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
        ASSERT_TRUE(disc) << err.toStdString();
        MigrationPlan plan = RoadmapMigrate::planFrom(*disc, QStringLiteral("Demo"),
                                                      QStringLiteral("demo"));
        RoadmapMigrate::validatePaths(plan, root);
        *out = RoadmapMigrateLoad::load(*store, plan, opts);
        ASSERT_TRUE(out->ok) << out->error.toStdString();

        RoadmapRender::Options ropts;
        ropts.liveRoadmapPath = QStringLiteral("ROADMAP.md");
        const auto rendered =
            RoadmapRender::render(*store, out->projectId, root, ropts, &err);
        ASSERT_TRUE(rendered) << err.toStdString();
        ASSERT_TRUE(rendered->gateFailures.isEmpty())
            << rendered->gateFailures.join(QStringLiteral("; ")).toStdString();
        ASSERT_TRUE(rendered->committed);
    };

    const auto driftReport = [](const QHash<qint64, RoadmapStore::ItemWrite> &from,
                                const QHash<qint64, RoadmapStore::ItemWrite> &to) {
        const auto b = byId(from);
        const auto a = byId(to);
        QStringList out;
        for (auto i = b.constBegin(); i != b.constEnd(); ++i) {
            const auto j = a.constFind(i.key());
            if (j == a.constEnd()) { out.append(i.key() + QStringLiteral(":<gone>")); continue; }
            const QStringList cols = governedDrift(i.value(), *j);
            if (!cols.isEmpty())
                out.append(i.key() + QLatin1Char(':') + cols.join(QLatin1Char('+')));
        }
        return out;
    };

    // --- cycle 1: migrate the hand-written source, then render it ---------
    RoadmapMigrateLoad::Outcome first;
    cycle(&first);
    const auto afterFirst = store->readItems(first.projectId, &err);
    ASSERT_TRUE(afterFirst) << err.toStdString();

    // --- cycle 2: import what cycle 1 rendered, then render again ---------
    RoadmapMigrateLoad::Outcome second;
    cycle(&second);
    const auto afterSecond = store->readItems(second.projectId, &err);
    ASSERT_TRUE(afterSecond) << err.toStdString();

    EXPECT_EQ(second.itemsInserted, 0)
        << "the re-import failed to match an item it had just written";
    EXPECT_EQ(second.itemsOrphaned, 0);

    // The settling, and the ONLY column allowed to show it. Between these two
    // stores the source changed from the hand-written file to cycle 1's render,
    // which materialised the `Kind:` line § 3.5 requires onto the two bullets
    // that lacked a canonical one (DEMO-0072 declared none, DEMO-0073 declared
    // `bugfix`). `body` holds a bullet's continuation lines verbatim, so it
    // grows by that line. Any OTHER governed column moving here is real drift,
    // which is why this is asserted per column rather than skipped.
    for (const QString &d : driftReport(*afterFirst, *afterSecond)) {
        const QStringList parts = d.split(QLatin1Char(':'));
        EXPECT_EQ(parts.value(1), QStringLiteral("body"))
            << "a governed column other than `body` moved while the render was "
               "still settling: " << d.toStdString();
    }

    // --- cycle 3: the gate. Nothing at all may move now ------------------
    RoadmapMigrateLoad::Outcome third;
    cycle(&third);
    const auto afterThird = store->readItems(third.projectId, &err);
    ASSERT_TRUE(afterThird) << err.toStdString();
    EXPECT_EQ(third.itemsUpdatedGoverned, 0)
        << "§ 2.6's governed-column counter is non-zero on a settled project";
    EXPECT_EQ(third.itemsUpdated, 0);
    const QStringList late = driftReport(*afterSecond, *afterThird);
    EXPECT_TRUE(late.isEmpty())
        << "the round trip is drifting rather than settled: "
        << late.join(QStringLiteral(", ")).toStdString();
}
