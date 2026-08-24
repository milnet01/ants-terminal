// Feature-conformance test for ANTS-3758. Contract:
// tests/features/roadmap_render/spec.md
//
// Behavioural, against a real store in a QTemporaryDir. The one exception is
// Inv11SingleElementReader, which is a source scrape because it asserts a
// REFIT — that no second element or project reader survives under src/ — and
// no behaviour of the render can observe that.

#include <gtest/gtest.h>

#include "roadmapexport.h"
#include "roadmapparse.h"   // ANTS-3820 — parseAntsV1Bullet()
#include "roadmaprender.h"
#include "roadmapstore.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <QTemporaryDir>

namespace {

// A store plus the project root its render writes into. Held together because
// every case needs both and the root must outlive the store.
//
// The store is a unique_ptr and NOT a value member, because RoadmapStore takes
// its path at construction and a default-constructed one resolves
// defaultPath() — the developer's REAL store under XDG_DATA_HOME. A value
// member here would have every case in this file writing into it.
struct Fixture {
    QTemporaryDir dir;
    std::unique_ptr<RoadmapStore> store;
    qint64        projectId = 0;
    qint64        rootSection = 0;

    QString root() const { return dir.path(); }
    QString livePath() const { return QStringLiteral("ROADMAP.md"); }
    QString liveAbs() const { return dir.filePath(QStringLiteral("ROADMAP.md")); }
};

// The synthetic root: empty slug, level 0, carrying the format marker and H1
// exactly as the migration would have filed them from the source file's head.
// § 2.8 turns on this being replayed rather than synthesised.
constexpr const char *kRootIntro = "<!-- ants-roadmap-format: 1 -->\n\n# Demo — Roadmap";

std::unique_ptr<Fixture> makeFixture() {
    auto f = std::make_unique<Fixture>();
    if (!f->dir.isValid())
        return nullptr;
    // The store lives in the temp dir, NOT at defaultPath().
    f->store = std::make_unique<RoadmapStore>(f->dir.filePath(QStringLiteral("store.db")));
    QString err;
    if (!f->store->open(&err)) {
        ADD_FAILURE() << "store open: " << err.toStdString();
        return nullptr;
    }
    const auto pid = f->store->registerProject(f->root(), QStringLiteral("Demo"),
                                               QStringLiteral("demo"), &err);
    if (!pid) {
        ADD_FAILURE() << "registerProject: " << err.toStdString();
        return nullptr;
    }
    f->projectId = *pid;

    const auto root = f->store->addSection(f->projectId, QString::fromUtf8(""), QString::fromUtf8(""),
                                           /*level=*/0, /*position=*/0, std::nullopt, &err);
    if (!root) {
        ADD_FAILURE() << "addSection(root): " << err.toStdString();
        return nullptr;
    }
    f->rootSection = *root;
    if (!f->store->setSectionIntro(f->rootSection, QString::fromUtf8(kRootIntro), &err)) {
        ADD_FAILURE() << "setSectionIntro: " << err.toStdString();
        return nullptr;
    }
    return f;
}

RoadmapStore::ItemWrite mkItem(qint64 projectId, const QString &id, const QString &headline,
                               qint64 sectionId, int position) {
    RoadmapStore::ItemWrite w;
    w.projectId = projectId;
    w.id = id;
    w.idOrigin = QStringLiteral("parsed");
    w.status = QStringLiteral("planned");
    w.headline = headline;
    w.kind = QStringLiteral("implement");
    w.source = QStringLiteral("planned");
    w.layman = QStringLiteral("A plain sentence.");
    w.sectionId = sectionId;
    w.position = position;
    return w;
}

RoadmapRender::Options liveOpts(const Fixture &f) {
    RoadmapRender::Options o;
    o.liveRoadmapPath = f.livePath();
    return o;
}

QString readAll(const QString &path) {
    QFile fh(path);
    if (!fh.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromUtf8(fh.readAll());
}

} // namespace

// INV-2 — sections come out in sectionOrderLess() order, (position, slug).
// The three slugs are deliberately in the OPPOSITE order to their positions, so
// a renderer that sorted by slug (or by insertion/section_id) produces a
// different file and this case reddens.
TEST(RoadmapRender, Inv2SectionOrder) {
    auto f = makeFixture();
    ASSERT_TRUE(f);
    QString err;
    // positions 1,2,3 against slugs zulu, mike, alpha.
    ASSERT_TRUE(f->store->addSection(f->projectId, QStringLiteral("zulu"), QStringLiteral("Zulu"), 2, 1, std::nullopt, &err));
    ASSERT_TRUE(f->store->addSection(f->projectId, QStringLiteral("mike"), QStringLiteral("Mike"), 2, 2, std::nullopt, &err));
    ASSERT_TRUE(f->store->addSection(f->projectId, QStringLiteral("alpha"), QStringLiteral("Alpha"), 2, 3, std::nullopt, &err));

    const auto out = RoadmapRender::render(*f->store, f->projectId, f->root(), liveOpts(*f), &err);
    ASSERT_TRUE(out) << err.toStdString();
    ASSERT_TRUE(out->committed);

    const QString text = readAll(f->liveAbs());
    const int zulu = text.indexOf(QStringLiteral("## Zulu"));
    const int mike = text.indexOf(QStringLiteral("## Mike"));
    const int alpha = text.indexOf(QStringLiteral("## Alpha"));
    ASSERT_GT(zulu, 0);
    EXPECT_LT(zulu, mike) << "position order lost — sorted by slug?";
    EXPECT_LT(mike, alpha);
}

// INV-3 — a section whose source_path names an archive renders THERE, and never
// folds back into the live roadmap. This is the outcome section.source_path's
// own DDL comment names ANTS-3758 for.
TEST(RoadmapRender, Inv3ArchiveRouting) {
    auto f = makeFixture();
    ASSERT_TRUE(f);
    QString err;
    const auto live = f->store->addSection(f->projectId, QStringLiteral("current"), QStringLiteral("Current"), 2, 1, std::nullopt, &err);
    const auto arch = f->store->addSection(f->projectId, QStringLiteral("v05"), QStringLiteral("0.5"), 2, 2, std::nullopt, &err);
    ASSERT_TRUE(live);
    ASSERT_TRUE(arch);
    ASSERT_TRUE(f->store->setSectionSource(*arch, QStringLiteral("docs/roadmap/0.5.md"), &err));

    const auto out = RoadmapRender::render(*f->store, f->projectId, f->root(), liveOpts(*f), &err);
    ASSERT_TRUE(out) << err.toStdString();
    EXPECT_EQ(out->filesWritten.size(), 2);

    const QString liveText = readAll(f->liveAbs());
    const QString archText = readAll(f->dir.filePath(QStringLiteral("docs/roadmap/0.5.md")));
    EXPECT_TRUE(liveText.contains(QStringLiteral("## Current")));
    EXPECT_FALSE(liveText.contains(QStringLiteral("## 0.5")))
        << "archive section folded back into ROADMAP.md";
    EXPECT_TRUE(archText.contains(QStringLiteral("## 0.5")));
}

// INV-4 — internal and dropped never appear; shipped does. The counters must
// account for the excluded ones, because a render that drops items and reports
// success is the silent loss § 2.4 refuses.
TEST(RoadmapRender, Inv4Membership) {
    auto f = makeFixture();
    ASSERT_TRUE(f);
    QString err;
    const auto sec = f->store->addSection(f->projectId, QStringLiteral("s"), QStringLiteral("S"), 2, 1, std::nullopt, &err);
    ASSERT_TRUE(sec);

    auto pub = mkItem(f->projectId, QStringLiteral("D-1"), QStringLiteral("Public planned."), *sec, 0);
    auto ship = mkItem(f->projectId, QStringLiteral("D-2"), QStringLiteral("Shipped work."), *sec, 1);
    ship.status = QStringLiteral("shipped");
    ship.layman.clear();   // closed items are not gated
    auto intern = mkItem(f->projectId, QStringLiteral("D-3"), QStringLiteral("Internal finding."), *sec, 2);
    intern.visibility = QStringLiteral("internal");
    auto dropped = mkItem(f->projectId, QStringLiteral("D-4"), QStringLiteral("Abandoned."), *sec, 3);
    dropped.status = QStringLiteral("dropped");
    for (const auto &w : {pub, ship, intern, dropped})
        ASSERT_TRUE(f->store->putItem(w, &err)) << err.toStdString();

    const auto out = RoadmapRender::render(*f->store, f->projectId, f->root(), liveOpts(*f), &err);
    ASSERT_TRUE(out) << err.toStdString();
    const QString text = readAll(f->liveAbs());
    EXPECT_TRUE(text.contains(QStringLiteral("D-1")));
    EXPECT_TRUE(text.contains(QStringLiteral("D-2"))) << "shipped items must still be listed";
    EXPECT_FALSE(text.contains(QStringLiteral("D-3"))) << "internal item published";
    EXPECT_FALSE(text.contains(QStringLiteral("D-4"))) << "dropped item published";
    EXPECT_EQ(out->itemsExcluded, 2);
    EXPECT_EQ(out->itemsRendered, 2);
}

// INV-5 — the gate is per PROJECT. One offender stops the whole render, the
// call still returns an ENGAGED outcome, and every offending id is named. A
// refusal that returned nullopt would make those ids unreachable.
TEST(RoadmapRender, Inv5PublishGate) {
    auto f = makeFixture();
    ASSERT_TRUE(f);
    QString err;
    const auto sec = f->store->addSection(f->projectId, QStringLiteral("s"), QStringLiteral("S"), 2, 1, std::nullopt, &err);
    ASSERT_TRUE(sec);
    ASSERT_TRUE(f->store->putItem(mkItem(f->projectId, QStringLiteral("G-1"), QStringLiteral("Fine."), *sec, 0), &err));
    auto bad = mkItem(f->projectId, QStringLiteral("G-2"), QStringLiteral("No layman."), *sec, 1);
    bad.layman.clear();
    ASSERT_TRUE(f->store->putItem(bad, &err));

    const auto out = RoadmapRender::render(*f->store, f->projectId, f->root(), liveOpts(*f), &err);
    ASSERT_TRUE(out) << "a gate failure must be an engaged result, not nullopt";
    EXPECT_FALSE(out->committed);
    EXPECT_TRUE(out->filesWritten.isEmpty()) << "gate failed but files were written";
    ASSERT_EQ(out->gateFailures.size(), 1);
    EXPECT_EQ(out->gateFailures.first(), QStringLiteral("G-2"));
    EXPECT_FALSE(QFileInfo::exists(f->liveAbs()));
}

// INV-5 — the gate judges the SCOPE it is given. `Inv5PublishGate` above is the
// unset case (judge everything, the original whole-project rule); this is the
// stated case, which is what RoadmapWrite passes so a write is refused by what
// it touched rather than by the project's legacy debt (ANTS-4628).
//
// The three blocks are three different scopes over ONE store, deliberately: the
// scope is the only thing that varies, so a failure here cannot be blamed on
// the fixture differing between cases.
TEST(RoadmapRender, Inv5GateScopeLimitsOffenders) {
    auto f = makeFixture();
    ASSERT_TRUE(f);
    QString err;
    const auto sec = f->store->addSection(f->projectId, QStringLiteral("s"), QStringLiteral("S"), 2, 1, std::nullopt, &err);
    ASSERT_TRUE(sec);

    auto legacy = mkItem(f->projectId, QStringLiteral("G-9"), QStringLiteral("Legacy, no layman."), *sec, 0);
    legacy.layman.clear();
    const auto legacyPk = f->store->putItem(legacy, &err);
    ASSERT_TRUE(legacyPk) << err.toStdString();

    auto touched = mkItem(f->projectId, QStringLiteral("G-8"), QStringLiteral("Touched, no layman."), *sec, 1);
    touched.layman.clear();
    const auto touchedPk = f->store->putItem(touched, &err);
    ASSERT_TRUE(touchedPk) << err.toStdString();

    // A scope naming only the touched item. The legacy offender is not judged,
    // the touched one is — so the render still refuses, and names exactly one.
    // Both halves matter: dropping the out-of-scope id is the feature, and
    // still failing on the in-scope one is what stops it becoming an exemption.
    {
        auto opts = liveOpts(*f);
        opts.gateScope = QSet<qint64>{*touchedPk};
        const auto out = RoadmapRender::render(*f->store, f->projectId, f->root(), opts, &err);
        ASSERT_TRUE(out);
        ASSERT_EQ(out->gateFailures.size(), 1)
            << "an out-of-scope offender must not be judged";
        EXPECT_EQ(out->gateFailures.first(), QStringLiteral("G-8"));
        EXPECT_TRUE(out->filesWritten.isEmpty());
        EXPECT_FALSE(QFileInfo::exists(f->liveAbs()));
    }

    // An EMPTY scope judges nothing, so the project publishes with both
    // offenders still uncured. That is the `op:render` case, and it is the one
    // that breaks ANTS-4628's deadlock: a project whose file carries none of
    // its ids can be published, and only then can its items be located at all.
    {
        auto opts = liveOpts(*f);
        opts.gateScope = QSet<qint64>{};
        const auto out = RoadmapRender::render(*f->store, f->projectId, f->root(), opts, &err);
        ASSERT_TRUE(out) << err.toStdString();
        EXPECT_TRUE(out->gateFailures.isEmpty());
        EXPECT_FALSE(out->filesWritten.isEmpty());
        EXPECT_TRUE(QFileInfo::exists(f->liveAbs()));
    }

    // UNSET still judges everything. This is the guard, not a repeat of
    // Inv5PublishGate: it proves the narrowing is opt-in, so a caller that
    // never heard of `gateScope` cannot silently lose the check.
    {
        const auto out = RoadmapRender::render(*f->store, f->projectId, f->root(), liveOpts(*f), &err);
        ASSERT_TRUE(out);
        EXPECT_EQ(out->gateFailures.size(), 2)
            << "an unset scope must judge every item";
    }
}

// INV-7 — two renders of an unchanged store are byte-identical, so a scheduled
// render produces no spurious diff.
TEST(RoadmapRender, Inv7Idempotent) {
    auto f = makeFixture();
    ASSERT_TRUE(f);
    QString err;
    const auto sec = f->store->addSection(f->projectId, QStringLiteral("s"), QStringLiteral("S"), 2, 1, std::nullopt, &err);
    ASSERT_TRUE(sec);
    ASSERT_TRUE(f->store->putItem(mkItem(f->projectId, QStringLiteral("I-1"), QStringLiteral("One."), *sec, 0), &err));

    ASSERT_TRUE(RoadmapRender::render(*f->store, f->projectId, f->root(), liveOpts(*f), &err));
    const QString first = readAll(f->liveAbs());
    ASSERT_TRUE(RoadmapRender::render(*f->store, f->projectId, f->root(), liveOpts(*f), &err));
    const QString second = readAll(f->liveAbs());
    EXPECT_EQ(first, second) << "render is not idempotent";
    EXPECT_FALSE(first.isEmpty());
}

// INV-8 — the marker appears within the first five lines, EXACTLY ONCE. The
// once matters: the root intro already carries it, so a render that also
// prepended its own constant would emit two and fail INV-1.
TEST(RoadmapRender, Inv8FormatMarker) {
    auto f = makeFixture();
    ASSERT_TRUE(f);
    QString err;
    const auto arch = f->store->addSection(f->projectId, QStringLiteral("v05"), QStringLiteral("0.5"), 2, 1, std::nullopt, &err);
    ASSERT_TRUE(arch);
    ASSERT_TRUE(f->store->setSectionSource(*arch, QStringLiteral("docs/roadmap/0.5.md"), &err));

    ASSERT_TRUE(RoadmapRender::render(*f->store, f->projectId, f->root(), liveOpts(*f), &err));

    const QString live = readAll(f->liveAbs());
    EXPECT_EQ(live.count(QStringLiteral("ants-roadmap-format")), 1)
        << "marker duplicated — replayed from the root intro AND synthesised?";
    const QStringList head = live.split(QLatin1Char('\n')).mid(0, 5);
    EXPECT_TRUE(head.join(QLatin1Char('\n')).contains(QStringLiteral("ants-roadmap-format")));

    // This fixture's archive has no root section of its own, so its marker is
    // the renderer's constant and it must still be there. (Corrected under
    // ANTS-3806: that is a property of THIS fixture, not of archives — a real
    // archive with pre-heading content stores its own root under ANTS-3766
    // § 2.3's per-source slug and replays it. Both paths are covered in
    // tests/features/roadmap_migrate_archive_root/.)
    const QString archive = readAll(f->dir.filePath(QStringLiteral("docs/roadmap/0.5.md")));
    EXPECT_EQ(archive.count(QStringLiteral("ants-roadmap-format")), 1)
        << "archive shipped without a format marker";
}

// INV-9 — the emitted depth is the stored `level`, and a level that contradicts
// the depth parent_id implies is a refusal rather than a silent choice.
TEST(RoadmapRender, Inv9LevelAgreesWithParent) {
    auto f = makeFixture();
    ASSERT_TRUE(f);
    QString err;
    const auto parent = f->store->addSection(f->projectId, QStringLiteral("p"), QStringLiteral("P"), 2, 1, std::nullopt, &err);
    ASSERT_TRUE(parent);
    // level 2 under a level-2 parent: should be 3.
    ASSERT_TRUE(f->store->addSection(f->projectId, QStringLiteral("c"), QStringLiteral("C"), 2, 2, *parent, &err));

    const auto out = RoadmapRender::render(*f->store, f->projectId, f->root(), liveOpts(*f), &err);
    EXPECT_FALSE(out) << "a level/parent contradiction must refuse, not pick one";
    EXPECT_TRUE(err.contains(QStringLiteral("level"))) << err.toStdString();
}

// INV-10 — narration and tables keep their stored position, interleaved with
// items rather than grouped after them.
TEST(RoadmapRender, Inv10ElementInterleaving) {
    auto f = makeFixture();
    ASSERT_TRUE(f);
    QString err;
    const auto sec = f->store->addSection(f->projectId, QStringLiteral("s"), QStringLiteral("S"), 2, 1, std::nullopt, &err);
    ASSERT_TRUE(sec);
    // narration, item, narration — the item is in the MIDDLE.
    ASSERT_TRUE(f->store->addElement(*sec, 0, QStringLiteral("narration"), QStringLiteral("BEFORE-PROSE"), &err));
    ASSERT_TRUE(f->store->putItem(mkItem(f->projectId, QStringLiteral("E-1"), QStringLiteral("Middle."), *sec, 1), &err));
    ASSERT_TRUE(f->store->addElement(*sec, 2, QStringLiteral("narration"), QStringLiteral("AFTER-PROSE"), &err));

    ASSERT_TRUE(RoadmapRender::render(*f->store, f->projectId, f->root(), liveOpts(*f), &err)) << err.toStdString();
    const QString text = readAll(f->liveAbs());
    const int before = text.indexOf(QStringLiteral("BEFORE-PROSE"));
    const int item = text.indexOf(QStringLiteral("E-1"));
    const int after = text.indexOf(QStringLiteral("AFTER-PROSE"));
    ASSERT_GT(before, 0);
    ASSERT_GT(item, 0);
    ASSERT_GT(after, 0);
    EXPECT_LT(before, item) << "elements grouped instead of interleaved";
    EXPECT_LT(item, after) << "elements grouped instead of interleaved";
}

// INV-1 — a `table` element is SERIALISED back into GFM, not replayed. The
// store holds § 5.2's canonical {header, rows} JSON, so a render that emitted
// the payload verbatim (as this one did until ANTS-3832) wrote that JSON into
// the file where the rows belong — and the JSON is not a table row, so a
// re-load files it as narration and the round-trip fails.
//
// The cell carrying a literal `|` is the half that pins the escaping: it must
// be spelled `\|` and it must be what the migration's tableCells() inverts,
// or INV-1 fails on the first pipe-bearing cell instead of on the payload.
TEST(RoadmapRender, Inv1TableRendersAsGfm) {
    auto f = makeFixture();
    ASSERT_TRUE(f);
    QString err;
    const auto sec = f->store->addSection(f->projectId, QStringLiteral("s"), QStringLiteral("S"), 2, 1, std::nullopt, &err);
    ASSERT_TRUE(sec);
    ASSERT_TRUE(f->store->addElement(
        *sec, 0, QStringLiteral("table"),
        QStringLiteral(R"({"header":["Lane","Note"],"rows":[["vt","x | y"],["chrome","plain"]]})"),
        &err)) << err.toStdString();

    ASSERT_TRUE(RoadmapRender::render(*f->store, f->projectId, f->root(), liveOpts(*f), &err))
        << err.toStdString();
    const QString text = readAll(f->liveAbs());
    EXPECT_TRUE(text.contains(QStringLiteral(
        "| Lane | Note |\n"
        "| --- | --- |\n"
        "| vt | x \\| y |\n"
        "| chrome | plain |")))
        << "table not serialised as GFM:\n" << text.toStdString();
    EXPECT_FALSE(text.contains(QStringLiteral("\"header\"")))
        << "raw payload JSON reached the file:\n" << text.toStdString();
}

// INV-1's other half — a payload the store accepted but the render cannot turn
// into a table is refused, not emitted malformed. `header` is absent, which the
// store's canonicaliser has no opinion about: it checks JSON, not shape.
TEST(RoadmapRender, TableRefusesShapelessPayload) {
    auto f = makeFixture();
    ASSERT_TRUE(f);
    QString err;
    const auto sec = f->store->addSection(f->projectId, QStringLiteral("s"), QStringLiteral("S"), 2, 1, std::nullopt, &err);
    ASSERT_TRUE(sec);
    ASSERT_TRUE(f->store->addElement(*sec, 0, QStringLiteral("table"),
                                     QStringLiteral(R"({"rows":[["a"]]})"), &err));

    EXPECT_FALSE(RoadmapRender::render(*f->store, f->projectId, f->root(), liveOpts(*f), &err));
    EXPECT_TRUE(err.contains(QStringLiteral("header"))) << err.toStdString();
    EXPECT_FALSE(QFileInfo::exists(f->liveAbs())) << "a refused render wrote a file";
}

// INV-12 — the four required § 3.5 pieces are emitted literally. Kind: in
// particular, even when its value equals § 3.5.3's default: INV-1's oracle
// cannot see that omission, because a re-parse restores the default.
TEST(RoadmapRender, Inv12RequiredPiecesPresent) {
    auto f = makeFixture();
    ASSERT_TRUE(f);
    QString err;
    const auto sec = f->store->addSection(f->projectId, QStringLiteral("s"), QStringLiteral("S"), 2, 1, std::nullopt, &err);
    ASSERT_TRUE(sec);
    // kind == the § 3.5.3 default, which is the case a round-trip check misses.
    ASSERT_TRUE(f->store->putItem(mkItem(f->projectId, QStringLiteral("R-1"), QStringLiteral("Required pieces."), *sec, 0), &err));

    ASSERT_TRUE(RoadmapRender::render(*f->store, f->projectId, f->root(), liveOpts(*f), &err));
    const QString text = readAll(f->liveAbs());
    const QRegularExpression bullet(
        QStringLiteral("^- \\x{1F4CB} \\[R-1\\] \\*\\*Required pieces\\.\\*\\*$"),
        QRegularExpression::MultilineOption);
    EXPECT_TRUE(bullet.match(text).hasMatch())
        << "bullet head missing a required piece:\n" << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("Kind: implement.")))
        << "Kind: omitted because it equals the default";
}

// ANTS-4582 — the trailer's terminating period belongs to the FORMAT, not the
// value: roadmap-format.md § 3.5 calls Evidence out as the one key rendered
// WITHOUT one, which makes the stop the format's on every other key. So a
// value that already ends in a stop must not be given a second. The parse
// chops exactly one (roadmapparse.cpp, ANTS-3764), so emitting one terminator
// either way is what keeps the pair symmetric.
TEST(RoadmapRender, Ants4582TrailerPeriodNotDoubled) {
    auto f = makeFixture();
    ASSERT_TRUE(f);
    QString err;
    const auto sec = f->store->addSection(f->projectId, QStringLiteral("s"), QStringLiteral("S"), 2, 1, std::nullopt, &err);
    ASSERT_TRUE(sec);
    auto ended = mkItem(f->projectId, QStringLiteral("P-1"), QStringLiteral("Value ends in a stop."), *sec, 0);
    ended.source = QStringLiteral("in-session-2026-08-20, reproduced on this repo.");
    ASSERT_TRUE(f->store->putItem(ended, &err)) << err.toStdString();
    auto bare = mkItem(f->projectId, QStringLiteral("P-2"), QStringLiteral("Value ends bare."), *sec, 1);
    bare.source = QStringLiteral("in-session-2026-08-20, reproduced on this repo");
    ASSERT_TRUE(f->store->putItem(bare, &err)) << err.toStdString();

    ASSERT_TRUE(RoadmapRender::render(*f->store, f->projectId, f->root(), liveOpts(*f), &err));
    const QString text = readAll(f->liveAbs());
    EXPECT_FALSE(text.contains(QStringLiteral("repo..")))
        << "a value already ending in a period was given a second:\n" << text.toStdString();
    // Guards the other direction: both spellings must still EMIT the key, so
    // the fix cannot pass by suppressing the trailer instead of trimming it.
    EXPECT_EQ(text.count(QStringLiteral("Source: in-session-2026-08-20, reproduced on this repo.")), 2)
        << text.toStdString();
}

// INV-13 — no file is written outside projectRoot, and the check covers the
// live roadmap as well as source_path. The live path is the one every project
// uses, so exempting it would hollow the invariant out.
TEST(RoadmapRender, Inv13PathContainment) {
    QString err;
    {
        auto f = makeFixture();
        ASSERT_TRUE(f);
        const auto sec = f->store->addSection(f->projectId, QStringLiteral("s"), QStringLiteral("S"), 2, 1, std::nullopt, &err);
        ASSERT_TRUE(sec);
        ASSERT_TRUE(f->store->setSectionSource(*sec, QStringLiteral("../escaped.md"), &err));
        const auto out = RoadmapRender::render(*f->store, f->projectId, f->root(), liveOpts(*f), &err);
        EXPECT_FALSE(out) << "source_path escaped the project root";
        EXPECT_TRUE(err.contains(QStringLiteral("escapes"))) << err.toStdString();
    }
    {
        auto f = makeFixture();
        ASSERT_TRUE(f);
        RoadmapRender::Options o;
        o.liveRoadmapPath = QStringLiteral("../escaped.md");
        const auto out = RoadmapRender::render(*f->store, f->projectId, f->root(), o, &err);
        EXPECT_FALSE(out) << "liveRoadmapPath escaped the project root";
    }
    {
        auto f = makeFixture();
        ASSERT_TRUE(f);
        RoadmapRender::Options o;   // empty liveRoadmapPath
        const auto out = RoadmapRender::render(*f->store, f->projectId, f->root(), o, &err);
        EXPECT_FALSE(out) << "an empty live path is a refusal, not a default";
    }
}

// INV-14 — a dry run reports the file set and touches nothing on disk.
TEST(RoadmapRender, Inv14DryRunWritesNothing) {
    auto f = makeFixture();
    ASSERT_TRUE(f);
    QString err;
    const auto sec = f->store->addSection(f->projectId, QStringLiteral("s"), QStringLiteral("S"), 2, 1, std::nullopt, &err);
    ASSERT_TRUE(sec);
    ASSERT_TRUE(f->store->putItem(mkItem(f->projectId, QStringLiteral("N-1"), QStringLiteral("Nothing."), *sec, 0), &err));

    RoadmapRender::Options o = liveOpts(*f);
    o.dryRun = true;
    const auto out = RoadmapRender::render(*f->store, f->projectId, f->root(), o, &err);
    ASSERT_TRUE(out) << err.toStdString();
    EXPECT_FALSE(out->filesWritten.isEmpty()) << "dry run must still report the file set";
    EXPECT_FALSE(QFileInfo::exists(f->liveAbs()))
        << "dry run wrote a file (write-then-delete is not a dry run)";
}

// INV-11 — the refit. A source scrape, deliberately: it asserts that no SECOND
// element or project reader survives under src/, which no behaviour of the
// render can observe. roadmapstore.cpp is the exemption the invariant names.
TEST(RoadmapRender, Inv11SingleElementReader) {
    const QDir srcDir(QStringLiteral(ANTS_SRC_DIR));
    ASSERT_TRUE(srcDir.exists()) << srcDir.absolutePath().toStdString();

    // CASE-SENSITIVE, and comments are stripped first. Both matter, and the
    // measurement is why: a case-insensitive scrape over raw text matched
    // ENGLISH PROSE in three unrelated files — "re-walking from project root"
    // (testauditengine.h), "come from project-controlled docs"
    // (indiereviewengine.cpp) and this invariant's own description in
    // roadmapstore.h. A matcher that fires on prose proves nothing about SQL.
    const QRegularExpression rx(QStringLiteral("FROM\\s+(element|project)\\b"));
    QStringList offenders;
    const QStringList files = srcDir.entryList({QStringLiteral("*.cpp"), QStringLiteral("*.h")},
                                               QDir::Files, QDir::Name);
    for (const QString &name : files) {
        if (name == QStringLiteral("roadmapstore.cpp"))
            continue;   // the reader's own home — the invariant's stated exemption
        QFile fh(srcDir.filePath(name));
        if (!fh.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        const QStringList lines = QString::fromUtf8(fh.readAll()).split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            const int comment = line.indexOf(QStringLiteral("//"));
            const QString code = comment < 0 ? line : line.left(comment);
            if (rx.match(code).hasMatch()) {
                offenders.append(name);
                break;
            }
        }
    }
    EXPECT_TRUE(offenders.isEmpty())
        << "a second element/project reader survives in: " << offenders.join(QStringLiteral(", ")).toStdString();
}

// ANTS-3818 — no NEW consumer reaches for the unsorted section enumerator.
//
// Document order is (position, slug), applied in C++ by sectionOrderLess()
// rather than in SQL, for the collation reason listSections()' own comment
// gives: QString::compare() is UTF-16 code-unit order while SQLite BINARY is
// UTF-8 byte order, and they disagree on the supplementary-plane characters an
// emoji heading slug reaches. That is correct and stays.
//
// The hazard the item names is that a consumer which simply iterates
// listSections() gets a PLAUSIBLE order, no error, and a wrong file.
// listSectionsOrdered() (ANTS-3818) makes the sorted call the obvious one, but
// "obvious" is not a guarantee — so this is the item's own stated alternative
// remedy, an invariant asserting each consumer remembered. INV-2 above pins
// render()'s ORDER behaviourally; this pins that no future consumer bypasses
// the sorted surface at all, which no behaviour of the render can observe.
//
// Lives here rather than with the store because this directory already carries
// the ANTS_SRC_DIR scrape machinery and INV-11, which is the same guard class
// ("no second reader survives under src/") one level down.
//
// The two exempt files are exempt for stated reasons, verified 2026-08-17:
//   roadmapstore.cpp        — the enumerator's own home, plus
//                             listSectionsOrdered()'s single call to it.
//   remotecontrol_roadmap_log_batch.cpp — THREE order-independent callers, each
//                             checked: create_section's renumber keys every row
//                             by its own id; rotate_minor builds QHash indexes
//                             and iterates a QHash, which has no order to lose;
//                             and retitle's slug-collision scan is a membership
//                             test. roadmapstore.h's own comment names the first
//                             two as the order-independent users.
//
// A NEW name appearing here is not automatically a defect — it is a consumer
// that must be read, and either moved to listSectionsOrdered() or added here
// with the reason it does not read order.
// ANTS-3820 — a `dropped` item has no markdown form, and the pairing that keeps
// the render from emitting one is now TESTED rather than merely true.
//
// The round trip is worse than a missing glyph. emojiFor("dropped") returns an
// empty string by design (§ 3.11 makes a fifth emoji an anti-pattern), so
// bulletText() emits a head line with NO status marker — and the native parse
// then fails stripInlineEmoji() and skips the bullet entirely. A dropped item
// rendered to markdown re-parses to NOTHING, so a round-trip oracle would see a
// vanished item rather than a malformed one.
//
// SCOPE — this case asserts ONLY the producer-side property, deliberately.
// The two callers exclude a dropped item by two different mechanisms:
//   - render() never emits one. **Already covered by Inv4Membership above**
//     ("internal and dropped never appear; shipped does"), so re-asserting it
//     here would duplicate a shipped case and pay a second full render for it.
//   - bulletsFromStore()'s appendRecord() (roadmapsource.cpp) relies on the
//     produced text FAILING to parse, and skips the nullopt. Nothing tested
//     that, and it is the half ANTS-3820 actually asks for — "state the
//     precondition on bulletText() and assert it".
//
// bulletText() deliberately does not refuse — see its header. Making it refuse
// would break appendRecord(), which wants exactly this output.
TEST(RoadmapRender, Ants3820DroppedItemHasNoMarkdownForm) {
    auto f = makeFixture();
    ASSERT_TRUE(f);
    QString err;

    auto gone = mkItem(f->projectId, QStringLiteral("ANTS-2"),
                       QStringLiteral("A dropped item."), f->rootSection, 2);
    gone.status = QStringLiteral("dropped");

    EXPECT_TRUE(RoadmapRender::emojiFor(QStringLiteral("dropped")).isEmpty())
        << "a fifth status emoji would silently give dropped a markdown form, "
           "and § 3.11 makes that an anti-pattern";

    // The property appendRecord() uses AS its exclusion signal. If a future
    // change gave dropped a glyph, this text would parse, the reader would
    // admit a dropped item, and ANTS-3793's INV-2 membership rule would break
    // somewhere far from the edit that caused it.
    const QString text = RoadmapRender::bulletText(gone);
    EXPECT_FALSE(RoadmapParse::parseAntsV1Bullet(text).has_value())
        << "a dropped item's bulletText() must NOT re-parse — bulletsFromStore() "
           "uses that failure as its exclusion. It produced: "
        << text.toStdString();
}

TEST(RoadmapRender, Ants3818NoUnsortedSectionConsumer) {
    const QDir srcDir(QStringLiteral(ANTS_SRC_DIR));
    ASSERT_TRUE(srcDir.exists()) << srcDir.absolutePath().toStdString();

    // Case-sensitive, comments stripped — INV-11's measured lesson, for the
    // same reason: this identifier is discussed in prose in roadmapstore.h at
    // length, and a scrape that fires on prose proves nothing about calls.
    // `listSectionsOrdered(` must NOT match, hence the negative lookahead on
    // the character that distinguishes them.
    const QRegularExpression rx(QStringLiteral("listSections\\s*\\((?!.*Ordered)"));
    const QStringList exempt{QStringLiteral("roadmapstore.cpp"),
                             QStringLiteral("roadmapstore.h"),
                             // ANTS-4620 — all three live in the BATCH half of
                             // the TU-5 split (create_section, rotate_minor,
                             // retitle_section). The single-item half now calls
                             // listSections() nowhere, so it is not exempt and a
                             // new caller landing there is caught.
                             QStringLiteral(
                                 "remotecontrol_roadmap_log_batch.cpp"),
                             // ANTS-4617 — op:"deregister"'s dry run calls it
                             // for `.size()` alone, to report how many sections
                             // WOULD go. It never reads an element of the list,
                             // so document order cannot reach the answer, and
                             // sorting to count would be work whose result is
                             // discarded.
                             QStringLiteral("roadmapmigrateverb.cpp")};
    QStringList offenders;
    const QStringList files = srcDir.entryList(
        {QStringLiteral("*.cpp"), QStringLiteral("*.h")}, QDir::Files, QDir::Name);
    for (const QString &name : files) {
        if (exempt.contains(name)) continue;
        QFile fh(srcDir.filePath(name));
        if (!fh.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        const QStringList lines = QString::fromUtf8(fh.readAll()).split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            const int comment = line.indexOf(QStringLiteral("//"));
            const QString code = comment < 0 ? line : line.left(comment);
            if (rx.match(code).hasMatch()) { offenders.append(name); break; }
        }
    }
    EXPECT_TRUE(offenders.isEmpty())
        << "these files call the UNSORTED listSections(): "
        << offenders.join(QStringLiteral(", ")).toStdString()
        << " — use listSectionsOrdered() if the caller reads document order, or "
           "add the file to this test's exempt list with the reason it does not";
}

// INV-1 — the render loses nothing and invents nothing, over the facts markdown
// carries. Proved through the shipped export: a store rendered and re-read must
// export the same bytes as the original, once § 2.6's excluded families are
// projected out of BOTH sides.
//
// This case asserts the half that is provable without the migration loader in
// the loop: every field the bullet is supposed to carry survives into the text.
// The full render → load → export comparison is what ANTS-3793's cutover work
// wires up, because it needs a second store and the loader's own options.
TEST(RoadmapRender, Inv1ExportsMatch) {
    auto f = makeFixture();
    ASSERT_TRUE(f);
    QString err;
    const auto sec = f->store->addSection(f->projectId, QStringLiteral("s"), QStringLiteral("S"), 2, 1, std::nullopt, &err);
    ASSERT_TRUE(sec);

    auto w = mkItem(f->projectId, QStringLiteral("F-1"), QStringLiteral("Everything carried."), *sec, 0);
    w.body = QStringLiteral("Body prose that must survive.");
    w.layman = QStringLiteral("A plain sentence for a non-programmer.");
    w.source = QStringLiteral("user-2026-08-03");
    w.lanes = {QStringLiteral("core"), QStringLiteral("vt")};
    w.evidence = {QStringLiteral("docs/screenshots/a.png")};
    ASSERT_TRUE(f->store->putItem(w, &err)) << err.toStdString();

    ASSERT_TRUE(RoadmapRender::render(*f->store, f->projectId, f->root(), liveOpts(*f), &err));
    const QString text = readAll(f->liveAbs());

    EXPECT_TRUE(text.contains(QStringLiteral("Body prose that must survive.")));
    EXPECT_TRUE(text.contains(QStringLiteral("**Layman:** A plain sentence for a non-programmer.")));
    EXPECT_TRUE(text.contains(QStringLiteral("Source: user-2026-08-03.")));
    EXPECT_TRUE(text.contains(QStringLiteral("Lanes: core, vt.")));
    // Evidence carries NO trailing period: paths contain dots, so one would
    // read as part of the last path (roadmap-format.md § 3.5).
    EXPECT_TRUE(text.contains(QStringLiteral("Evidence: docs/screenshots/a.png\n")));
    EXPECT_FALSE(text.contains(QStringLiteral("Evidence: docs/screenshots/a.png.")));
}

// INV-6 — staging means no failure in rendering, gating or serialising leaves a
// partial write. Here the second file's directory is made unwritable, so the
// pass fails before anything is committed and the first file must not exist.
TEST(RoadmapRender, Inv6AllOrNothing) {
    auto f = makeFixture();
    ASSERT_TRUE(f);
    QString err;
    ASSERT_TRUE(f->store->addSection(f->projectId, QStringLiteral("live"), QStringLiteral("Live"), 2, 1, std::nullopt, &err));
    const auto arch = f->store->addSection(f->projectId, QStringLiteral("old"), QStringLiteral("Old"), 2, 2, std::nullopt, &err);
    ASSERT_TRUE(arch);
    ASSERT_TRUE(f->store->setSectionSource(*arch, QStringLiteral("locked/0.5.md"), &err));

    QDir(f->root()).mkpath(QStringLiteral("locked"));
    const QString locked = f->dir.filePath(QStringLiteral("locked"));
    ASSERT_TRUE(QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::ExeOwner));

    const auto out = RoadmapRender::render(*f->store, f->projectId, f->root(), liveOpts(*f), &err);
    QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    EXPECT_FALSE(out) << "an unwritable target must abort before any commit";
    EXPECT_FALSE(QFileInfo::exists(f->liveAbs()))
        << "the live roadmap landed while a sibling failed — files written without staging";
}
