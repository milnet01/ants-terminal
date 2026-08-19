// Feature-conformance test for ANTS-3808. Contract:
// tests/features/roadmap_item_body/spec.md
//
// Behavioural, against a real store in a QTemporaryDir, driving the PUBLIC
// entry point RoadmapRender::render() rather than the § 2.4 export — that
// exercises the bytes which actually reach the file, which is the stronger
// assertion. Inv2SingleGrammar is the one source scrape, because it asserts a
// REFIT (that no second trailer-key grammar survives under src/) and no
// behaviour of the render or the reader can observe that.

#include <gtest/gtest.h>

#include "roadmapmigrate.h"
#include "roadmapparse.h"
#include "roadmaprender.h"
#include "roadmapstore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <memory>
#include <optional>

namespace {

// ANTS-3833 — the basenames of the eleven RemoteControl TUs, DERIVED from the
// declared list rather than hard-coded, so a twelfth TU needs no edit here and
// cannot be exempted by accident: a file absent from ANTS_RC_SOURCES is not on
// this list however it is named.
QStringList rcTranslationUnits() {
    QStringList out;
    const QStringList paths = QString::fromUtf8(ANTS_RC_SOURCES)
                                  .split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString &p : paths)
        out.append(QFileInfo(p).fileName());
    return out;
}

// A store plus the project root its render writes into.
//
// The store is a unique_ptr and NOT a value member, because RoadmapStore takes
// its path at construction and a default-constructed one resolves
// defaultPath() — the developer's REAL store under XDG_DATA_HOME. A value
// member here would have every case in this file writing into it.
struct Fixture {
    QTemporaryDir dir;
    std::unique_ptr<RoadmapStore> store;
    qint64 projectId = 0;
    qint64 section = 0;

    QString root() const { return dir.path(); }
    QString liveAbs() const { return dir.filePath(QStringLiteral("ROADMAP.md")); }
};

std::unique_ptr<Fixture> makeFixture() {
    auto f = std::make_unique<Fixture>();
    if (!f->dir.isValid())
        return nullptr;
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
    // The synthetic root's slug and title are EMPTY, not null: a default-
    // constructed QString binds as SQL NULL and section.slug is NOT NULL.
    const auto root = f->store->addSection(f->projectId, QString::fromUtf8(""),
                                           QString::fromUtf8(""),
                                           /*level=*/0, /*position=*/0, std::nullopt, &err);
    if (!root) {
        ADD_FAILURE() << "addSection(root): " << err.toStdString();
        return nullptr;
    }
    if (!f->store->setSectionIntro(*root,
            QStringLiteral("<!-- ants-roadmap-format: 1 -->\n\n# Demo — Roadmap"), &err)) {
        ADD_FAILURE() << "setSectionIntro: " << err.toStdString();
        return nullptr;
    }
    const auto sec = f->store->addSection(f->projectId, QStringLiteral("work"),
                                          QStringLiteral("Work"), 2, 1, std::nullopt, &err);
    if (!sec) {
        ADD_FAILURE() << "addSection(work): " << err.toStdString();
        return nullptr;
    }
    f->section = *sec;
    return f;
}

// Put ONE item in its own store and render it. One item per file is what makes
// whole-file occurrence counting meaningful in Inv1: two items sharing a `kind`
// would each be correct and the file would still hold the value twice.
QString renderOne(RoadmapStore::ItemWrite w) {
    auto f = makeFixture();
    if (!f)
        return QString();
    QString err;
    w.projectId = f->projectId;
    w.sectionId = f->section;
    w.position  = 0;
    if (!f->store->putItem(w, &err)) {
        ADD_FAILURE() << "putItem: " << err.toStdString();
        return QString();
    }
    RoadmapRender::Options o;
    o.liveRoadmapPath = QStringLiteral("ROADMAP.md");
    const auto out = RoadmapRender::render(*f->store, f->projectId, f->root(), o, &err);
    if (!out) {
        ADD_FAILURE() << "render: " << err.toStdString();
        return QString();
    }
    if (!out->gateFailures.isEmpty()) {
        ADD_FAILURE() << "render gate: " << out->gateFailures.join(QStringLiteral(", ")).toStdString();
        return QString();
    }
    QFile fh(f->liveAbs());
    if (!fh.open(QIODevice::ReadOnly | QIODevice::Text)) {
        ADD_FAILURE() << "could not read the rendered file";
        return QString();
    }
    return QString::fromUtf8(fh.readAll());
}

int occurrences(const QString &haystack, const QString &needle) {
    if (needle.isEmpty())
        return 0;
    int n = 0;
    for (qsizetype at = haystack.indexOf(needle); at >= 0;
         at = haystack.indexOf(needle, at + needle.size()))
        ++n;
    return n;
}

// An open item needs a `layman` or ANTS-3758's INV-5 gate refuses the whole
// render, so every fixture below carries one.
RoadmapStore::ItemWrite baseItem(const QString &id, const QString &headline) {
    RoadmapStore::ItemWrite w;
    w.id       = id;
    w.idOrigin = QStringLiteral("parsed");
    w.status   = QStringLiteral("planned");
    w.headline = headline;
    w.kind     = QStringLiteral("implement");
    return w;
}

// (a) — a migrated bullet whose residual carries every key at the SAME value,
// so all five columns suppress and the body is the only source in the output.
RoadmapStore::ItemWrite alphaItem() {
    RoadmapStore::ItemWrite w = baseItem(QStringLiteral("DEMO-0001"),
                                         QStringLiteral("Alpha headline sentinel"));
    w.source   = QStringLiteral("in-session-alpha");
    w.layman   = QStringLiteral("Alpha stays put");
    w.lanes    = {QStringLiteral("chrome"), QStringLiteral("tests")};
    w.evidence = {QStringLiteral("docs/alpha.png")};
    w.body     = QStringLiteral("Kind: implement.\n"
                                "Source: in-session-alpha.\n"
                                "Lanes: chrome, tests.\n"
                                "**Layman:** Alpha stays put.\n"
                                "Evidence: docs/alpha.png");
    return w;
}

RoadmapMigrate::Discovery discoveryOf(const QString &path, const QString &markdown) {
    RoadmapMigrate::Discovery d;
    RoadmapMigrate::Source s;
    s.path     = path;
    s.markdown = markdown;
    s.format   = RoadmapParse::detectRoadmapFormat(markdown.split(QLatin1Char('\n')));
    d.sources.append(s);
    return d;
}

const RoadmapMigrate::PlannedItem *itemWithHeadline(const RoadmapMigrate::MigrationPlan &plan,
                                                    const QString &headline) {
    for (const RoadmapMigrate::PlannedItem &it : plan.items)
        if (it.headline == headline)
            return &it;
    return nullptr;
}

// ANTS-4506 — one migrate-then-render cycle over a single-bullet document.
// The store write carries `provenance` verbatim: § 2.4 gates the `Source:`
// line on `provenance.source != "defaulted"`, so dropping it here would make
// every fixture lose its Source line for a reason that has nothing to do with
// the strip.
RoadmapStore::ItemWrite writeOf(const RoadmapMigrate::PlannedItem &p) {
    RoadmapStore::ItemWrite w;
    w.id         = p.id;
    w.idOrigin   = p.idOrigin;
    w.status     = p.status;
    w.headline   = p.headline;
    w.kind       = p.kind;
    w.source     = p.source;
    w.layman     = p.layman;
    w.body       = p.body;
    w.lanes      = p.lanes;
    w.evidence   = p.evidence;
    w.extras     = p.extras;
    w.provenance = p.provenance;
    return w;
}

// The ONE bullet a fixture document declares, as the migration would file it.
std::optional<RoadmapMigrate::PlannedItem> planOne(const QString &markdown) {
    const auto plan = RoadmapMigrate::planFrom(discoveryOf(QStringLiteral("ROADMAP.md"), markdown),
                                               QStringLiteral("Demo"), QStringLiteral("demo"));
    if (plan.items.size() != 1) {
        ADD_FAILURE() << "expected exactly one planned item, got " << plan.items.size();
        return std::nullopt;
    }
    return plan.items.at(0);
}

// The bullet's own lines out of a rendered file — its `- ` line plus every
// indented continuation. INV-6's byte-identity clause is about the BULLET; the
// fixture store synthesises its own preamble and headings around it.
QString bulletBlock(const QString &fileText) {
    QStringList out;
    const QStringList lines = fileText.split(QLatin1Char('\n'));
    for (const QString &l : lines) {
        if (out.isEmpty()) {
            if (l.startsWith(QStringLiteral("- ")))
                out.append(l);
            continue;
        }
        if (!l.startsWith(QStringLiteral("  ")))
            break;
        out.append(l);
    }
    return out.join(QLatin1Char('\n'));
}

// migrate → store → render, returning the rendered file.
QString cycleOnce(const QString &markdown) {
    const auto p = planOne(markdown);
    if (!p)
        return QString();
    return renderOne(writeOf(*p));
}

} // namespace

// INV-1 — a rendered bullet contains its headline exactly once and each trailer
// key's CANONICAL VALUE exactly once. Values, not key literals: § 2.3.1's
// no-suppression branch legitimately writes the key twice, so a key-counting
// assertion would fail against a correct implementation.
TEST(RoadmapItemBody, Inv1NoDuplication) {
    // (a) suppression fires for every key.
    const QString a = renderOne(alphaItem());
    ASSERT_FALSE(a.isEmpty());
    EXPECT_EQ(occurrences(a, QStringLiteral("Alpha headline sentinel")), 1)
        << "the headline is rendered twice — the migration stored the render's "
           "own head-line prefix in item.body:\n" << a.toStdString();
    EXPECT_EQ(occurrences(a, QStringLiteral("implement")), 1) << a.toStdString();
    EXPECT_EQ(occurrences(a, QStringLiteral("in-session-alpha")), 1) << a.toStdString();
    EXPECT_EQ(occurrences(a, QStringLiteral("chrome, tests")), 1) << a.toStdString();
    EXPECT_EQ(occurrences(a, QStringLiteral("Alpha stays put")), 1) << a.toStdString();
    EXPECT_EQ(occurrences(a, QStringLiteral("docs/alpha.png")), 1) << a.toStdString();

    // (b) EMPTY residual — every column is emitted unsuppressed, and "exactly
    // once" is then the whole assertion. This is the row that reddens if the
    // suppression fires on a key the body does not carry.
    RoadmapStore::ItemWrite b = baseItem(QStringLiteral("DEMO-0002"),
                                         QStringLiteral("Bravo headline sentinel"));
    b.source   = QStringLiteral("in-session-bravo");
    b.layman   = QStringLiteral("Bravo stays put");
    b.lanes    = {QStringLiteral("vt")};
    b.evidence = {QStringLiteral("docs/bravo.png")};
    const QString bt = renderOne(b);
    ASSERT_FALSE(bt.isEmpty());
    EXPECT_EQ(occurrences(bt, QStringLiteral("Bravo headline sentinel")), 1) << bt.toStdString();
    EXPECT_EQ(occurrences(bt, QStringLiteral("Kind: implement.")), 1) << bt.toStdString();
    EXPECT_EQ(occurrences(bt, QStringLiteral("in-session-bravo")), 1) << bt.toStdString();
    EXPECT_EQ(occurrences(bt, QStringLiteral("Lanes: vt.")), 1) << bt.toStdString();
    EXPECT_EQ(occurrences(bt, QStringLiteral("Bravo stays put")), 1) << bt.toStdString();
    EXPECT_EQ(occurrences(bt, QStringLiteral("docs/bravo.png")), 1) << bt.toStdString();

    // (c) the two-value case, and the shadowing mention is MID-LINE. ANTS-4505
    // moved suppression to presence, so a LINE-INITIAL `Source:` in the body
    // now suppresses whatever its value — the fixture would stop
    // discriminating, because the key literal would appear once and a
    // key-counting assertion would pass too. § 2.3.1's third branch (present
    // only mid-line) is the one shape that still reaches the no-suppression
    // branch, so the column is emitted and the key LITERAL appears twice —
    // correct output that a key-counting invariant would reject.
    RoadmapStore::ItemWrite c = baseItem(QStringLiteral("DEMO-0003"),
                                         QStringLiteral("Charlie headline sentinel"));
    c.source = QStringLiteral("charlie-current-note");
    c.layman = QStringLiteral("Charlie stays put");
    c.body   = QStringLiteral("Filed after the audit; Source: charlie-earlier-note.");
    const QString ct = renderOne(c);
    ASSERT_FALSE(ct.isEmpty());
    EXPECT_EQ(occurrences(ct, QStringLiteral("charlie-current-note")), 1) << ct.toStdString();
    EXPECT_EQ(occurrences(ct, QStringLiteral("charlie-earlier-note")), 1) << ct.toStdString();
    EXPECT_EQ(occurrences(ct, QStringLiteral("Source:")), 2)
        << "the canonical column must still be emitted when the body disagrees, "
           "or the STALE value becomes the only one in the file:\n" << ct.toStdString();
}

// INV-2 — RoadmapParse remains the only bullet grammar in src/, outside the
// enumerated exemptions. Case-sensitive, comments stripped, and keyed on FILE
// and SYMBOL rather than on line number: a test encoding a literal line number
// rots on the first unrelated edit above it and then fails for a reason that
// has nothing to do with the invariant.
TEST(RoadmapItemBody, Inv2SingleGrammar) {
    const QDir srcDir(QStringLiteral(ANTS_SRC_DIR));
    ASSERT_TRUE(srcDir.exists()) << srcDir.absolutePath().toStdString();

    // A SITE is one QRegularExpression CONSTRUCTION — `QRegularExpression <name>(`.
    // Requiring the name is what keeps `const QRegularExpression &rxCommitSha()`
    // from being counted a second time as its own return type, and what keeps
    // `QRegularExpression::CaseInsensitiveOption` out entirely.
    const QRegularExpression rxCtor(QStringLiteral("\\bQRegularExpression\\s+(\\w+)\\s*\\("));
    // The trailer key INSIDE a regex — not the plain "Kind: " output literals
    // the render legitimately emits, which a naive scrape hits. This is the
    // shape ANTS-3758's INV-11 had to be corrected into after matching English
    // prose.
    const QRegularExpression rxKey(QStringLiteral("(Kind|Lanes|Layman|Evidence|Source):"));

    QHash<QString, int> hitsPerFile;
    const QStringList files = srcDir.entryList({QStringLiteral("*.cpp"), QStringLiteral("*.h")},
                                               QDir::Files, QDir::Name);
    ASSERT_FALSE(files.isEmpty());
    for (const QString &name : files) {
        QFile fh(srcDir.filePath(name));
        if (!fh.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        const QStringList lines = QString::fromUtf8(fh.readAll()).split(QLatin1Char('\n'));
        QStringList stripped;
        stripped.reserve(lines.size());
        for (const QString &line : lines) {
            const qsizetype comment = line.indexOf(QStringLiteral("//"));
            stripped.append(comment < 0 ? line : line.left(comment));
        }
        const QString code = stripped.join(QLatin1Char('\n'));
        auto it = rxCtor.globalMatch(code);
        while (it.hasNext()) {
            const auto m = it.next();
            // The constructor arguments: to the end of the statement.
            const qsizetype semi = code.indexOf(QLatin1Char(';'), m.capturedEnd());
            const QString args = code.mid(m.capturedEnd(),
                                          (semi < 0 ? code.size() : semi) - m.capturedEnd());
            if (rxKey.match(args).hasMatch())
                hitsPerFile[name] += 1;
        }
    }

    // The grammar itself — uncapped, and asserted NON-ZERO so a matcher that
    // silently stopped matching cannot report a clean tree.
    EXPECT_GT(hitsPerFile.value(QStringLiteral("roadmapparse.cpp")), 0)
        << "the scrape matched nothing in the grammar itself — it is broken, "
           "not the tree";

    // The exempt sites in the RemoteControl implementation, which cannot be
    // otherwise: both rxBoldLayman constructions capture the Layman sentence
    // INCLUDING its trailing period (rec.layman is period-stripped by
    // ANTS-1154 INV-4, and a period-less CHANGELOG body was the bug ANTS-1933
    // fixed), and rxCommitSha embeds `\bSource:` as a lead-in it skips past
    // rather than a value it extracts.
    //
    // ANTS-3833 — counted across the ELEVEN TUs rather than against one
    // filename. The split moved these three bodies to sibling TUs without
    // touching a byte of them, and a per-file exemption reads that as three new
    // offenders. The number is what this asserts; which TU holds each site is
    // the thing the split is free to change.
    int rcHits = 0;
    for (const QString &tu : rcTranslationUnits())
        rcHits += hitsPerFile.value(tu);
    EXPECT_EQ(rcHits, 3);

    // The site this spec REMOVES. Asserting zero here rather than treating the
    // inventory as the allowlist is the whole point: an allowlist that included
    // this file would stop checking the one deliverable INV-2 exists to force.
    EXPECT_EQ(hitsPerFile.value(QStringLiteral("roadmapdialog.cpp")), 0)
        << "the dialog grew its own trailer-key regex again — it must ask "
           "RoadmapParse::trailerValuesIn()";

    QStringList offenders;
    for (auto it = hitsPerFile.constBegin(); it != hitsPerFile.constEnd(); ++it) {
        if (it.key() == QStringLiteral("roadmapparse.cpp")
            || rcTranslationUnits().contains(it.key()))
            continue;
        if (it.value() > 0)
            offenders.append(it.key() + QStringLiteral(" (") + QString::number(it.value())
                             + QStringLiteral(")"));
    }
    offenders.sort();
    EXPECT_TRUE(offenders.isEmpty())
        << "a second bullet grammar survives in: "
        << offenders.join(QStringLiteral(", ")).toStdString();
}

// INV-3 — re-parsing a rendered bullet yields the same five trailer values the
// store holds, for every item reachable without a shadowing consumer write.
// Fixture 3 is deliberately OUTSIDE that scope and asserts the opposite
// direction: where a column and its body have been separated, the FILE wins.
TEST(RoadmapItemBody, Inv3RenderReaderAgree) {
    struct Case {
        const char *what;
        RoadmapStore::ItemWrite item;
    };
    // A post-cutover item whose residual carries NO trailer key, so every
    // column is emitted rather than suppressed.
    RoadmapStore::ItemWrite post = baseItem(QStringLiteral("DEMO-0009"),
                                            QStringLiteral("Delta headline sentinel"));
    post.source   = QStringLiteral("in-session-delta");
    post.layman   = QStringLiteral("Delta stays put");
    post.lanes    = {QStringLiteral("vt"), QStringLiteral("chrome")};
    post.evidence = {QStringLiteral("docs/delta.png")};
    post.body     = QStringLiteral("Residual prose with no metadata in it.");

    const Case cases[] = {
        {"migrated — the residual carries the keys, suppression fires", alphaItem()},
        {"post-cutover — the residual carries none, columns are emitted", post},
    };

    for (const Case &c : cases) {
        SCOPED_TRACE(c.what);
        const QString text = renderOne(c.item);
        ASSERT_FALSE(text.isEmpty());
        const QVector<RoadmapParse::BulletRecord> back = RoadmapParse::parseBullets(text);
        const RoadmapParse::BulletRecord *rec = nullptr;
        for (const RoadmapParse::BulletRecord &r : back)
            if (r.id == c.item.id)
                rec = &r;
        ASSERT_TRUE(rec) << "the rendered bullet did not parse back:\n" << text.toStdString();
        EXPECT_EQ(rec->kind, c.item.kind) << text.toStdString();
        EXPECT_EQ(rec->source, c.item.source) << text.toStdString();
        EXPECT_EQ(rec->layman, c.item.layman) << text.toStdString();
        EXPECT_EQ(rec->lanes, c.item.lanes) << text.toStdString();
        EXPECT_EQ(rec->evidence, c.item.evidence) << text.toStdString();
    }

    // Fixture 3 — the ANTS-4505 discriminator. The body declares `Kind:`
    // line-initially with a value the column disagrees with; suppression fires
    // anyway, so the body's line is the only `Kind:` in the file and the
    // re-parse adopts it. Asserting store-equality here reds against a correct
    // implementation: the file is the authoring surface, so where the two have
    // been separated the file wins and the store follows. Written straight into
    // the store, because `roadmap_log` refuses this shape (`body_shadowed`).
    RoadmapStore::ItemWrite split = baseItem(QStringLiteral("DEMO-0010"),
                                             QStringLiteral("Echo headline sentinel"));
    split.layman = QStringLiteral("Echo stays put");
    split.kind   = QStringLiteral("implement");
    split.source = QStringLiteral("in-session-echo");
    split.body   = QStringLiteral("Kind: fix.");
    const QString st = renderOne(split);
    ASSERT_FALSE(st.isEmpty());
    EXPECT_EQ(occurrences(st, QStringLiteral("Kind:")), 1)
        << "presence suppression did not fire on a line-initial declaration:\n"
        << st.toStdString();
    const QVector<RoadmapParse::BulletRecord> back = RoadmapParse::parseBullets(st);
    const RoadmapParse::BulletRecord *rec = nullptr;
    for (const RoadmapParse::BulletRecord &r : back)
        if (r.id == split.id)
            rec = &r;
    ASSERT_TRUE(rec) << st.toStdString();
    EXPECT_EQ(rec->kind, QStringLiteral("fix"))
        << "the body's own declaration must be what the re-parse yields — a "
           "human correcting the file has to be able to move the column:\n"
        << st.toStdString();
}

// INV-4 — trailerValuesIn(body) equals what parseBullets() assigns from the same
// body, over § 2.2.1's normalisation table. Without this equality the render's
// suppression compares incommensurable values, never fires, and the defect
// stays live behind a passing spec.
TEST(RoadmapItemBody, Inv4AccessorAgrees) {
    // Trims — and `lanes.value` is the RAW capture, with neither the trim nor
    // the period chop, because parseBullets() splits it before applying either.
    {
        const QString body = QStringLiteral("Kind:   implement  .\n"
                                            "Lanes: chrome ,  tests .");
        const auto tv = RoadmapParse::trailerValuesIn(body);
        EXPECT_EQ(tv.kind.value, QStringLiteral("implement"));
        EXPECT_EQ(tv.lanes.value, QStringLiteral("chrome ,  tests"));
        EXPECT_EQ(tv.lanesList,
                  (QStringList{QStringLiteral("chrome"), QStringLiteral("tests")}));
    }
    // Evidence: trimmed and period-chopped BEFORE the split, and `value` is the
    // text the split is applied to — not symmetric with lanes.
    {
        const auto tv = RoadmapParse::trailerValuesIn(
            QStringLiteral("Evidence: docs/a.png, docs/b.png."));
        EXPECT_EQ(tv.evidence.value, QStringLiteral("docs/a.png, docs/b.png"));
        EXPECT_EQ(tv.evidenceList,
                  (QStringList{QStringLiteral("docs/a.png"), QStringLiteral("docs/b.png")}));
    }
    // ONE trailing period, and not when the value ends `..` — dots inside paths
    // are content.
    {
        const auto tv = RoadmapParse::trailerValuesIn(QStringLiteral("Evidence: docs/a.."));
        EXPECT_EQ(tv.evidence.value, QStringLiteral("docs/a.."));
    }
    // Source stops at a FOLLOWING trailer key: ten corpus lines write two keys
    // on one line.
    {
        const auto tv = RoadmapParse::trailerValuesIn(
            QStringLiteral("Source: regression. Lanes: packaging."));
        EXPECT_EQ(tv.source.value, QStringLiteral("regression"));
        EXPECT_EQ(tv.lanesList, (QStringList{QStringLiteral("packaging")}));
    }
    // ANTS-3722's backtick guard — a bullet that QUOTES a trailer key is
    // talking ABOUT it, never declaring it.
    {
        const auto tv = RoadmapParse::trailerValuesIn(
            QStringLiteral("The `Lanes:` key is documented here."));
        EXPECT_EQ(tv.lanes.offset, -1);
        EXPECT_TRUE(tv.lanesList.isEmpty());
    }
    // offset is the CAPTURE's start, in QString positions — which is what a
    // consumer quoting the value needs. Asserted by reading the value back out
    // of the body at that index, so a byte offset would fail here.
    // anchored needs BOTH polarities or it asserts nothing: computed off the
    // capture rather than the match it is false on every key of every bullet,
    // and a fixture set that only ever expects false passes against that.
    {
        const QString body = QStringLiteral("Kind: implement.\n"
                                            "Filed after the audit; Source: in-session-x.");
        const auto tv = RoadmapParse::trailerValuesIn(body);
        ASSERT_GE(tv.kind.offset, 0);
        EXPECT_EQ(body.mid(tv.kind.offset, tv.kind.value.size()), tv.kind.value);
        EXPECT_TRUE(tv.kind.anchored) << "a line-leading Kind: must report anchored";
        ASSERT_GE(tv.source.offset, 0);
        EXPECT_EQ(body.mid(tv.source.offset, tv.source.value.size()), tv.source.value);
        EXPECT_FALSE(tv.source.anchored)
            << "an inline mid-prose Source: must NOT report anchored — computed "
               "off the capture instead of the match, this field is always false";
        // A non-ASCII bullet: UTF-16 code units, not bytes.
        const QString wide = QStringLiteral("Résumé — ünicode prose. Source: in-session-y.");
        const auto wtv = RoadmapParse::trailerValuesIn(wide);
        ASSERT_GE(wtv.source.offset, 0);
        EXPECT_EQ(wide.mid(wtv.source.offset, wtv.source.value.size()), wtv.source.value);
    }
    // And the equality itself, against the reader, over one body carrying all
    // five keys.
    {
        const QString markdown = QStringLiteral(
            "<!-- ants-roadmap-format: 1 -->\n\n"
            "- 📋 [DEMO-0007] **Accessor parity.**\n"
            "  Kind: implement.\n"
            "  Source: in-session-parity.\n"
            "  Lanes: chrome, tests.\n"
            "  **Layman:** It agrees.\n"
            "  Evidence: docs/a.png, docs/b.png\n");
        const auto recs = RoadmapParse::parseBullets(markdown);
        ASSERT_EQ(recs.size(), 1);
        const auto tv = RoadmapParse::trailerValuesIn(recs.at(0).body);
        EXPECT_EQ(tv.kind.value, recs.at(0).kind);
        EXPECT_EQ(tv.source.value, recs.at(0).source);
        EXPECT_EQ(tv.layman.value, recs.at(0).layman);
        EXPECT_EQ(tv.lanesList, recs.at(0).lanes);
        EXPECT_EQ(tv.evidenceList, recs.at(0).evidence);
    }
}

// INV-5 — no bullet text is lost across migrate-then-render, over § 2.1's four
// shapes. Two of them carry the weight: the headline-plus-continuations row is
// what a naive join gets wrong, and the GFM row is what proves headlineEnd is
// set on every headline branch rather than only the bold one.
TEST(RoadmapItemBody, Inv5NoBodyLoss) {
    const QString antsV1 = QStringLiteral(
        "<!-- ants-roadmap-format: 1 -->\n"
        "\n"
        "# Demo — Roadmap\n"
        "\n"
        "## Work\n"
        "\n"
        "- 📋 [DEMO-0001] **Headline only.**\n"
        "- 📋 [DEMO-0002] **Has continuations.**\n"
        "  Continuation prose line.\n"
        "  Kind: implement.\n"
        "  Layman: A sentence.\n"
        "- 📋 [DEMO-0003] **Has trailing prose.** ROW3-TAIL-TEXT stays.\n"
        "  Kind: implement.\n");
    const auto plan = RoadmapMigrate::planFrom(discoveryOf(QStringLiteral("ROADMAP.md"), antsV1),
                                               QStringLiteral("Demo"), QStringLiteral("demo"));

    // Row 1 — headline only: the residual is EMPTY, which is a normal outcome
    // and not an error. § 2.3's suppression then fires for no key, so every
    // column is emitted exactly once.
    const auto *r1 = itemWithHeadline(plan, QStringLiteral("Headline only."));
    ASSERT_TRUE(r1);
    EXPECT_EQ(r1->body, QString()) << "expected an empty residual, got: " << r1->body.toStdString();

    // Row 2 — headline + continuations: the continuations ONLY, with NO leading
    // newline. An empty first-line residual kept as an empty string makes body
    // start with '\n', which is non-empty, so renderBullet()'s
    // `if (!it.body.isEmpty())` guard passes and emits a stray indented line on
    // nearly every bullet.
    // ANTS-4506 amended this row: the continuations LESS any stripped trailing
    // run, so the two trailer lines below the prose are gone and the prose is
    // what remains.
    const auto *r2 = itemWithHeadline(plan, QStringLiteral("Has continuations."));
    ASSERT_TRUE(r2);
    EXPECT_EQ(r2->body, QStringLiteral("Continuation prose line."));
    EXPECT_FALSE(r2->body.startsWith(QLatin1Char('\n')));

    // Row 3 — the case the naive "drop the first line" rule LOSES. The reader
    // takes the headline from the bold token only, so text after the closing
    // `**` lives nowhere but body's first line — true of somewhere between a
    // seventh and a third of this project's own bracket-id bullets, depending
    // on how a soft-wrapped bold headline is counted (ANTS-3808 § 2.1).
    const auto *r3 = itemWithHeadline(plan, QStringLiteral("Has trailing prose."));
    ASSERT_TRUE(r3);
    EXPECT_TRUE(r3->body.contains(QStringLiteral("ROW3-TAIL-TEXT stays.")))
        << "post-`**` text was deleted by the strip: " << r3->body.toStdString();
    EXPECT_EQ(r3->body, QStringLiteral("ROW3-TAIL-TEXT stays."));

    // Row 4 — GFM, where the headline IS the whole first line. A GFM bullet
    // writes neither `[id]` nor `**`, so an implementation recording the offset
    // only in the rxBold branch leaves it at -1 here, strips nothing, and the
    // duplication returns.
    const QString gfm = QStringLiteral(
        "# Demo — Roadmap\n"
        "\n"
        "## Work\n"
        "\n"
        "- [ ] GFM row four headline\n"
        "  ROW4-CONT line.\n");
    const auto gfmPlan = RoadmapMigrate::planFrom(
        discoveryOf(QStringLiteral("ROADMAP.md"), gfm),
        QStringLiteral("Demo"), QStringLiteral("demo"));
    const auto *r4 = itemWithHeadline(gfmPlan, QStringLiteral("GFM row four headline"));
    ASSERT_TRUE(r4) << "the GFM bullet did not plan as an item";
    EXPECT_EQ(r4->body, QStringLiteral("ROW4-CONT line."))
        << "the GFM head line was not stripped — headlineEnd is unset on that branch";
}

// INV-6 — migrate-then-render is an identity on the FIRST cycle and the stored
// body gains nothing. The general assertion is over the BODY; byte identity is
// the narrower clause and holds only where the bullet is already written in
// § 2.4's emission order and spelling, with two-space continuations and no text
// after the closing `**`.
TEST(RoadmapItemBody, Inv6RoundTripAddsNothing) {
    const auto docWith = [](const QString &bullet) {
        return QStringLiteral("<!-- ants-roadmap-format: 1 -->\n"
                              "\n"
                              "# Demo — Roadmap\n"
                              "\n"
                              "## Work\n"
                              "\n")
               + bullet + QLatin1Char('\n');
    };

    // (1) Trailers last — the conforming shape. The trailing run is the
    // render's own output, so the strip moves exactly the lines § 2.4 puts
    // back and the bullet round-trips BYTE for byte.
    //
    // The `**Layman:**` line carries NO trailing period, which is the render's
    // own spelling: `rec.layman` is period-stripped by ANTS-1154 INV-4, so a
    // Layman line written with one cannot round-trip byte-identically and a
    // fixture in author style would red against a correct implementation.
    {
        const QString bullet = QStringLiteral(
            "- 📋 [DEMO-0011] **Trailers last.**\n"
            "  Residual prose line.\n"
            "  **Layman:** It stays put\n"
            "  Kind: implement.\n"
            "  Source: in-session-one.\n"
            "  Lanes: chrome, tests.\n"
            "  Evidence: docs/one.png");
        const QString doc = docWith(bullet);
        const auto before = planOne(doc);
        ASSERT_TRUE(before);
        EXPECT_EQ(before->body, QStringLiteral("Residual prose line."))
            << "the trailing trailer run was not stripped from the stored body: "
            << before->body.toStdString();
        const QString rendered = cycleOnce(doc);
        ASSERT_FALSE(rendered.isEmpty());
        EXPECT_EQ(bulletBlock(rendered), bullet) << rendered.toStdString();
        const auto after = planOne(rendered);
        ASSERT_TRUE(after);
        EXPECT_EQ(after->body, before->body) << rendered.toStdString();
    }

    // (2) Prose BELOW the trailers — the run stops at the prose, so the trailer
    // lines stay in the body and § 2.3's suppression keeps the render from
    // adding a second copy. This is the fixture an implementation fails by
    // stripping every trailer line rather than the trailing run, and it reds
    // silently: the body still round-trips, it has just lost an authored line.
    {
        const QString bullet = QStringLiteral(
            "- 📋 [DEMO-0012] **Prose below the trailers.**\n"
            "  **Layman:** It keeps its authored line.\n"
            "  Kind: implement.\n"
            "  Authored prose after the trailers.");
        const QString doc = docWith(bullet);
        const auto before = planOne(doc);
        ASSERT_TRUE(before);
        EXPECT_EQ(before->body,
                  QStringLiteral("**Layman:** It keeps its authored line.\n"
                                 "Kind: implement.\n"
                                 "Authored prose after the trailers."))
            << "a trailer line ABOVE authored prose is the author's, not the "
               "render's: " << before->body.toStdString();
        const QString rendered = cycleOnce(doc);
        ASSERT_FALSE(rendered.isEmpty());
        const auto after = planOne(rendered);
        ASSERT_TRUE(after);
        EXPECT_EQ(after->body, before->body) << rendered.toStdString();
    }

    // (3) A body that is ENTIRELY trailer lines strips to empty — a normal
    // outcome (§ 2.1), not an error, and every column is then emitted.
    {
        const QString bullet = QStringLiteral(
            "- 📋 [DEMO-0013] **All trailer lines.**\n"
            "  **Layman:** Nothing but metadata\n"
            "  Kind: implement.");
        const QString doc = docWith(bullet);
        const auto before = planOne(doc);
        ASSERT_TRUE(before);
        EXPECT_EQ(before->body, QString()) << before->body.toStdString();
        const QString rendered = cycleOnce(doc);
        ASSERT_FALSE(rendered.isEmpty());
        EXPECT_EQ(bulletBlock(rendered), bullet) << rendered.toStdString();
    }

    // (4) The DISCRIMINATOR, and the only fixture that reds against the
    // *Breaks when* mutation. The first three keep their trailing trailer
    // lines with the strip omitted, those lines DECLARE their keys, § 2.3
    // suppresses every column line, and identity holds anyway. Here the
    // residual declares nothing line-initially — the trailers sit mid-sentence
    // — so the render emits its block, the next parse files it into `body`,
    // and the stored body grows. That is the +458 / +97 accretion measured on
    // this project's own corpus on 2026-08-19.
    {
        const QString bullet = QStringLiteral(
            "- 📋 [DEMO-0014] **Accretion discriminator.**\n"
            "  **Layman:** It grows.\n"
            "  Filed after the audit; Kind: implement. Source: in-session-four.");
        const QString doc = docWith(bullet);
        const auto before = planOne(doc);
        ASSERT_TRUE(before);
        EXPECT_EQ(before->body,
                  QStringLiteral("**Layman:** It grows.\n"
                                 "Filed after the audit; Kind: implement. "
                                 "Source: in-session-four."));
        const QString rendered = cycleOnce(doc);
        ASSERT_FALSE(rendered.isEmpty());
        const auto after = planOne(rendered);
        ASSERT_TRUE(after);
        EXPECT_EQ(after->body, before->body)
            << "the stored body gained the render's own trailer block:\n"
            << rendered.toStdString();
    }

    // (5) § 2.1's ONLY-DECLARATION condition. A stale line-initial `Kind: bug`
    // in a continuation and the canonical `Kind: implement.` at the tail: the
    // tail line must NOT be stripped, or the residual's last line-initial
    // `Kind:` becomes `bug`, presence suppression drops the canonical column
    // line, and the next migration adopts `bug` — a migrated item rewriting its
    // own column with no consumer write. The `kind` vocabulary rider is no help
    // here, because `bug` is recognised (it maps to `fix`).
    {
        const QString bullet = QStringLiteral(
            "- 📋 [DEMO-0015] **Only-declaration guard.**\n"
            "  **Layman:** It keeps its column.\n"
            "  Kind: bug\n"
            "  More prose about this item.\n"
            "  Kind: implement.");
        const QString doc = docWith(bullet);
        const auto before = planOne(doc);
        ASSERT_TRUE(before);
        EXPECT_EQ(before->kind, QStringLiteral("implement"));
        EXPECT_TRUE(before->body.contains(QStringLiteral("Kind: implement.")))
            << "the tail line is NOT the only line-initial Kind: declaration, "
               "so stripping it hands the column to the stale one: "
            << before->body.toStdString();
        const QString cycle1 = cycleOnce(doc);
        ASSERT_FALSE(cycle1.isEmpty());
        const auto after1 = planOne(cycle1);
        ASSERT_TRUE(after1);
        EXPECT_EQ(after1->kind, QStringLiteral("implement")) << cycle1.toStdString();
        const QString cycle2 = cycleOnce(cycle1);
        ASSERT_FALSE(cycle2.isEmpty());
        const auto after2 = planOne(cycle2);
        ASSERT_TRUE(after2);
        EXPECT_EQ(after2->kind, QStringLiteral("implement"))
            << "the column was rewritten by its own body across two cycles:\n"
            << cycle2.toStdString();
    }
}
