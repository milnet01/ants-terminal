// ANTS-3771 — feature-conformance test for a DECLARED id format. Contract:
// tests/features/roadmap_id_format_declared/spec.md; design:
// docs/specs/ANTS-3771-id-format-declaration.md
//
// Behavioural throughout, against real files in QTemporaryDirs: the whole
// point of the item is that a declaration on disk changes what the reader,
// the writer and the migration do, so a case that hands IdFormat straight to
// parseBullets() would prove the parser and not the feature. Only the cases
// that are about the parser alone do that, and they say so.

#include "projectsettings.h"
#include "remotecontrol.h"
#include "roadmapmigrate.h"
#include "roadmapmigrateload.h"
#include "roadmapparse.h"
#include "roadmapsource.h"
#include "roadmapstore.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>
#include <QVector>

#include <memory>

namespace {

using RoadmapParse::BulletRecord;
using RoadmapParse::IdFormat;

// The worked pattern from the design spec's § 3.1. `AX1. <headline>` yields
// `AX1`; a bare `A1` captures the whole span (the case an unchanged boldId
// would get wrong); prose does not match.
const char *kPattern = "^([A-Za-z]{1,4}\\d+)(?:\\.|$)";

bool writeFile(const QString &path, const QByteArray &text) {
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return f.write(text) == text.size();
}

// A Vestige-shaped GFM roadmap. Padded past kRoadmapMinParseableSize so the
// write verbs' unrecognised_format gate does not fire on a short fixture.
QByteArray gfmDoc() {
    QByteArray b = "# Roadmap\n\n";
    for (int i = 0; i < 30; ++i)
        b += "Padding to clear the minimum-parseable-size gate. \n";
    b +=
        "\n## Systems\n\n"
        // the 192-bullet Vestige shape: id and headline inside one bold span
        "- [ ] **AX1. Geometric ray-traced audio occlusion**\n"
        "\n"
        // a bare-token id whose capture spans the WHOLE span — INV-5's case
        "- [ ] **A1** \xE2\x80\x94 Bare token with an em-dash headline.\n"
        "\n"
        // the pattern must be applied to extractBoldId()'s OUTPUT, which has
        // already chopped the trailing `.` — INV-3
        "- [x] **B2.** Trailing period inside the bold span.\n"
        "\n"
        // prose the pattern does not match: keeps today's id, never emptied
        "- [ ] **Photo mode** \xE2\x80\x94 a prose lead-in, not an id.\n"
        "\n"
        // a matched lead-in whose body cites an unrelated id — INV-2
        "- [ ] **C3. Cited elsewhere** see [ANTS-9999] for context.\n"
        "\n";
    return b;
}

// Write `.ants/project.json` with the given id_format members. An empty member
// is omitted, so the undeclared case is a file with no key rather than a key
// with an empty value.
bool declare(const QString &root, const QString &prefix, const QString &pattern) {
    QJsonObject fmt;
    if (!prefix.isEmpty())  fmt[QStringLiteral("prefix")]  = prefix;
    if (!pattern.isEmpty()) fmt[QStringLiteral("pattern")] = pattern;
    QJsonObject o;
    o[QStringLiteral("id_format")] = fmt;
    return writeFile(root + QStringLiteral("/.ants/project.json"),
                     QJsonDocument(o).toJson());
}

const BulletRecord *byHeadlineContains(const QVector<BulletRecord> &bs,
                                       const char *needle) {
    for (const auto &b : bs)
        if (b.headline.contains(QString::fromUtf8(needle))) return &b;
    return nullptr;
}
const BulletRecord *byId(const QVector<BulletRecord> &bs, const char *id) {
    for (const auto &b : bs)
        if (b.id == QString::fromUtf8(id)) return &b;
    return nullptr;
}

std::unique_ptr<RoadmapStore> openStore(const QString &path,
                                        RoadmapStore::Access access) {
    // NEVER default-construct RoadmapStore: it resolves defaultPath(), the
    // developer's REAL store under XDG_DATA_HOME.
    auto store = std::make_unique<RoadmapStore>(
        path, RoadmapStore::kDefaultHistoryCapBytes, access);
    QString err;
    if (!store->open(&err))
        return nullptr;
    return store;
}

}  // namespace

// ---------------------------------------------------------------- INV-1 ----

// A project that declares nothing parses byte-identically to today. Asserted
// over the WHOLE record vector, not over `id` alone: the fields the
// declaration touches are id, idToken, boldId and headline, and checking one
// would miss the other three.
TEST(roadmap_id_format_declared, Inv1UndeclaredIsByteIdentical) {
    const QString md = QString::fromUtf8(gfmDoc());
    const auto before = RoadmapParse::parseBullets(md);
    const auto after  = RoadmapParse::parseBullets(md, IdFormat{});
    ASSERT_EQ(before.size(), after.size());
    ASSERT_GT(before.size(), 0);
    for (int i = 0; i < before.size(); ++i) {
        EXPECT_EQ(before.at(i).id,        after.at(i).id)        << "bullet " << i;
        EXPECT_EQ(before.at(i).idToken,   after.at(i).idToken)   << "bullet " << i;
        EXPECT_EQ(before.at(i).boldId,    after.at(i).boldId)    << "bullet " << i;
        EXPECT_EQ(before.at(i).headline,  after.at(i).headline)  << "bullet " << i;
        EXPECT_EQ(before.at(i).format,    after.at(i).format)    << "bullet " << i;
        EXPECT_EQ(before.at(i).synthetic, after.at(i).synthetic) << "bullet " << i;
    }
}

// A declaration whose every member is invalid reads as undeclared, because
// load() drops per entry. Goes through the FILE, so it is the loader's drop
// rule under test and not a hand-built IdFormat.
TEST(roadmap_id_format_declared, Inv1DroppedMembersReadAsUndeclared) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    // `2026` is letter-free (isValidIdPrefix rejects it); `([` does not compile.
    ASSERT_TRUE(declare(root, QStringLiteral("2026"), QStringLiteral("([")));
    const IdFormat fmt = ProjectSettings::idFormatFor(root);
    EXPECT_FALSE(fmt.isDeclared())
        << "INV-1: an id_format whose every member was dropped must read as "
           "undeclared, not as a half-declaration";
}

// ---------------------------------------------------------------- INV-2 ----

// A declared match wins over the body-wide rxId. Undeclared, the same bullet
// reports the citation.
TEST(roadmap_id_format_declared, Inv2MatchBeatsBodyWideRxId) {
    const QString md = QString::fromUtf8(gfmDoc());
    IdFormat fmt; fmt.pattern = QString::fromUtf8(kPattern);

    const auto plain = RoadmapParse::parseBullets(md);
    const auto *p = byHeadlineContains(plain, "for context");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->id, QStringLiteral("ANTS-9999"))
        << "precondition: undeclared, rxId matches body-wide and wins";

    const auto decl = RoadmapParse::parseBullets(md, fmt);
    const auto *d = byId(decl, "C3");
    ASSERT_NE(d, nullptr)
        << "INV-2: under a declaration the lead-in is authoritative and the "
           "citation is a citation";
}

// Every branch the declaration does not govern assigns what it assigns today.
// The ants-v1 native document is the check: a pattern that would match its
// bold tokens must change nothing there.
TEST(roadmap_id_format_declared, Inv2NativeBranchIsUntouched) {
    const QString md = QString::fromUtf8(
        "## Work\n"
        "\n"
        "- \xE2\x9C\x85 [ANTS-0001] **Normal bullet.**\n"
        "- \xF0\x9F\x93\x8B **Cl9.** Short headline here.\n"
        "- \xF0\x9F\x93\x8B [Cb7] **Bracketed headline.**\n");
    IdFormat fmt;
    fmt.pattern = QStringLiteral("^(.+)$");   // would match anything
    const auto plain = RoadmapParse::parseBullets(md);
    const auto decl  = RoadmapParse::parseBullets(md, fmt);
    ASSERT_EQ(plain.size(), decl.size());
    ASSERT_GT(plain.size(), 0);
    for (int i = 0; i < plain.size(); ++i) {
        EXPECT_EQ(plain.at(i).id,       decl.at(i).id)       << "bullet " << i;
        EXPECT_EQ(plain.at(i).boldId,   decl.at(i).boldId)   << "bullet " << i;
        EXPECT_EQ(plain.at(i).headline, decl.at(i).headline) << "bullet " << i;
    }
}

// ---------------------------------------------------------------- INV-3 ----

TEST(roadmap_id_format_declared, Inv3MatchSetsIdIdTokenAndEmptiesBoldId) {
    IdFormat fmt; fmt.pattern = QString::fromUtf8(kPattern);
    const auto bs = RoadmapParse::parseBullets(QString::fromUtf8(gfmDoc()), fmt);

    const auto *a = byId(bs, "AX1");
    ASSERT_NE(a, nullptr) << "INV-3: capture group 1 is the id";
    EXPECT_EQ(a->idToken, QStringLiteral("AX1"))
        << "INV-3: idToken is the captured id, not the whole bold run — "
           "leaving the run there quarantines the bullet the declaration "
           "just resolved";
    EXPECT_TRUE(a->boldId.isEmpty())
        << "INV-3: boldId means the reader ADOPTED the span by inference, and "
           "under a declaration it adopted nothing";
    EXPECT_EQ(a->headline,
              QStringLiteral("Geometric ray-traced audio occlusion"))
        << "INV-3: the text the pattern did not consume is the headline";
}

// The pattern is matched against extractBoldId()'s OUTPUT — the span with its
// one trailing `.` already chopped. Against the RAW span `B2.` the pattern
// above still matches (the `(?:\.|$)` arm), so the distinguishing assertion
// is the HEADLINE: chopped, nothing follows the id and the bullet falls back
// to today's headline path; raw, a stray `.` would survive into the capture's
// tail.
TEST(roadmap_id_format_declared, Inv3PatternSeesTheStrippedCandidate) {
    IdFormat fmt; fmt.pattern = QStringLiteral("^([A-Za-z0-9]+)$");
    const auto bs = RoadmapParse::parseBullets(QString::fromUtf8(gfmDoc()), fmt);
    const auto *b = byId(bs, "B2");
    ASSERT_NE(b, nullptr)
        << "INV-3: `^([A-Za-z0-9]+)$` matches `**B2.**` only against the "
           "STRIPPED candidate — against the raw span the trailing `.` "
           "defeats the anchor";
    EXPECT_TRUE(b->boldId.isEmpty());
}

// ---------------------------------------------------------------- INV-4 ----

TEST(roadmap_id_format_declared, Inv4NonMatchKeepsTodaysId) {
    const QString md = QString::fromUtf8(gfmDoc());
    IdFormat fmt; fmt.pattern = QString::fromUtf8(kPattern);
    const auto plain = RoadmapParse::parseBullets(md);
    const auto decl  = RoadmapParse::parseBullets(md, fmt);
    ASSERT_EQ(plain.size(), decl.size());

    const auto *p = byHeadlineContains(plain, "a prose lead-in");
    const auto *d = byHeadlineContains(decl,  "a prose lead-in");
    ASSERT_NE(p, nullptr);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(p->id, QStringLiteral("Photo mode"))
        << "precondition: today the prose lead-in IS adopted as the id";
    EXPECT_EQ(d->id, p->id)
        << "INV-4: a non-match keeps exactly the id the heuristic assigns";
    EXPECT_EQ(d->boldId, p->boldId);

    // And no bullet anywhere loses an id, which is the invariant's teeth: an
    // emptied id makes the bullet a narrator and roadmap_query drops it.
    for (int i = 0; i < plain.size(); ++i)
        if (!plain.at(i).id.isEmpty())
            EXPECT_FALSE(decl.at(i).id.isEmpty())
                << "INV-4: bullet " << i << " lost its id under a declaration";
}

// ---------------------------------------------------------------- INV-5 ----

// The whole-span capture. `**A1**` captures `A1`, so `id == boldId` would hold
// if boldId were left set, and idWasInferred() would fire on an id the project
// DECLARED. This is the case the design spec's loop 1 got wrong.
TEST(roadmap_id_format_declared, Inv5WholeSpanCaptureIsNotInferred) {
    const QString md = QString::fromUtf8(gfmDoc());
    IdFormat fmt; fmt.pattern = QString::fromUtf8(kPattern);

    const auto plain = RoadmapParse::parseBullets(md);
    const auto *p = byId(plain, "A1");
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(RoadmapParse::idWasInferred(*p))
        << "precondition: undeclared, the bare token is an inference";

    const auto decl = RoadmapParse::parseBullets(md, fmt);
    const auto *d = byId(decl, "A1");
    ASSERT_NE(d, nullptr);
    EXPECT_FALSE(RoadmapParse::idWasInferred(*d))
        << "INV-5: the capture spans the whole bold span, so an unchanged "
           "boldId would report a DECLARED id as inferred";
    EXPECT_EQ(d->headline,
              QStringLiteral("Bare token with an em-dash headline."))
        << "INV-5: emptying boldId must not cost the em-dash headline split";
}

TEST(roadmap_id_format_declared, Inv5NonMatchStillReportsInferred) {
    IdFormat fmt; fmt.pattern = QString::fromUtf8(kPattern);
    const auto bs = RoadmapParse::parseBullets(QString::fromUtf8(gfmDoc()), fmt);
    const auto *d = byId(bs, "Photo mode");
    ASSERT_NE(d, nullptr);
    EXPECT_TRUE(RoadmapParse::idWasInferred(*d))
        << "INV-5: the pattern did not match, so this id is still the "
           "reader's guess — which is exactly what ANTS-4491's converter "
           "reads to find the ones still needing a decision";
}

// The flag reaches the ENVELOPE, over a real declaring project on disk.
TEST(roadmap_id_format_declared, Inv5FieldReachesTheEnvelope) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"), gfmDoc()));
    ASSERT_TRUE(declare(root, QString(), QString::fromUtf8(kPattern)));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("status")]     = QStringLiteral("all");
    const QJsonObject resp = rc.cmdRoadmapQuery(req).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();

    const QJsonArray bullets = resp.value(QStringLiteral("bullets")).toArray();
    const QString dump = QString::fromUtf8(QJsonDocument(bullets).toJson());
    int matched = 0, unmatched = 0;
    for (const auto &v : bullets) {
        const QJsonObject o = v.toObject();
        const QString id = o.value(QStringLiteral("id")).toString();
        const bool has = o.contains(QStringLiteral("id_inferred"));
        if (id == QStringLiteral("AX1") || id == QStringLiteral("A1")) {
            ++matched;
            EXPECT_FALSE(has)
                << "INV-5: a declared id must not carry id_inferred\n"
                << dump.toStdString();
        } else if (id == QStringLiteral("Photo mode")) {
            ++unmatched;
            EXPECT_TRUE(has && o.value(QStringLiteral("id_inferred")).toBool())
                << "INV-5: a non-match is still an inference\n"
                << dump.toStdString();
        }
    }
    EXPECT_EQ(matched, 2)   << "fixture: two matched bullets\n" << dump.toStdString();
    EXPECT_EQ(unmatched, 1) << "fixture: one prose lead-in\n"   << dump.toStdString();
}

// ---------------------------------------------------------------- INV-7 ----

// A declared prefix beats the markdown sniff, and an explicit id_prefix
// argument still beats the declaration. Driven through roadmap_log op:append
// with dry_run, so the id that would be allocated is reported without writing.
TEST(roadmap_id_format_declared, Inv7DeclaredPrefixBeatsTheSniff) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    QByteArray md = "# Roadmap\n\n";
    for (int i = 0; i < 30; ++i)
        md += "Padding to clear the minimum-parseable-size gate. \n";
    md += "\n## Work\n\n- \xE2\x9C\x85 [SNIF-0001] **Existing bullet.**\n\n";
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"), md));
    ASSERT_TRUE(writeFile(root + QStringLiteral("/.roadmap-counter"), "1\n"));
    ASSERT_TRUE(declare(root, QStringLiteral("DECL"), QString()));

    const auto append = [&](const QString &explicitPrefix) {
        RemoteControl rc(nullptr);
        QJsonObject req;
        req[QStringLiteral("caller_cwd")] = root;
        req[QStringLiteral("op")]         = QStringLiteral("append");
        req[QStringLiteral("section")]    = QStringLiteral("work");
        req[QStringLiteral("status")]     = QStringLiteral("planned");
        req[QStringLiteral("headline")]   = QStringLiteral("A new item.");
        req[QStringLiteral("kind")]       = QStringLiteral("implement");
        req[QStringLiteral("source")]     = QStringLiteral("test");
        req[QStringLiteral("dry_run")]    = true;
        if (!explicitPrefix.isEmpty())
            req[QStringLiteral("id_prefix")] = explicitPrefix;
        return rc.cmdRoadmapLogAppendForTest(req).object();
    };

    const QJsonObject declared = append(QString());
    ASSERT_TRUE(declared.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(declared).toJson().toStdString();
    EXPECT_TRUE(declared.value(QStringLiteral("would_be_id")).toString()
                    .startsWith(QStringLiteral("DECL-")))
        << "INV-7: the declared prefix must beat the markdown sniff (SNIF)\n"
        << QJsonDocument(declared).toJson().toStdString();

    const QJsonObject overridden = append(QStringLiteral("ARGP"));
    ASSERT_TRUE(overridden.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(overridden).toJson().toStdString();
    EXPECT_TRUE(overridden.value(QStringLiteral("would_be_id")).toString()
                    .startsWith(QStringLiteral("ARGP-")))
        << "INV-7: an explicit id_prefix argument still wins\n"
        << QJsonDocument(overridden).toJson().toStdString();
}

// ---------------------------------------------------------------- INV-8 ----

TEST(roadmap_id_format_declared, Inv8WrittenIdRefusals) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    QByteArray md = "# Roadmap\n\n";
    for (int i = 0; i < 30; ++i)
        md += "Padding to clear the minimum-parseable-size gate. \n";
    md += "\n## Work\n\n- \xE2\x9C\x85 [DECL-0001] **Existing bullet.**\n\n";
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"), md));
    const QByteArray before = md;
    ASSERT_TRUE(declare(root, QStringLiteral("DECL"), QString()));

    const auto appendStable = [&](const QString &stableId) {
        RemoteControl rc(nullptr);
        QJsonObject req;
        req[QStringLiteral("caller_cwd")]  = root;
        req[QStringLiteral("op")]          = QStringLiteral("append");
        req[QStringLiteral("section")]     = QStringLiteral("work");
        req[QStringLiteral("status")]      = QStringLiteral("planned");
        req[QStringLiteral("headline")]    = QStringLiteral("A new item.");
        req[QStringLiteral("kind")]        = QStringLiteral("implement");
        req[QStringLiteral("source")]      = QStringLiteral("test");
        req[QStringLiteral("id_strategy")] = QStringLiteral("stable_prefix");
        req[QStringLiteral("stable_id")]   = stableId;
        return rc.cmdRoadmapLogAppendForTest(req).object();
    };

    // Row 1 — ANTS-3769's class. The prefix is right; the SUFFIX is not a
    // number, so neither the universal grammar nor a declared pattern (there
    // is none here) accepts it.
    const QJsonObject bad = appendStable(QStringLiteral("DECL-119x"));
    EXPECT_FALSE(bad.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(bad.value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_id_format"))
        << QJsonDocument(bad).toJson().toStdString();

    // Row 2 — well-formed, wrong project. Distinct code, because the fix is
    // different: this id is valid everywhere except here.
    const QJsonObject wrong = appendStable(QStringLiteral("OTHR-0007"));
    EXPECT_FALSE(wrong.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(wrong.value(QStringLiteral("code")).toString(),
              QStringLiteral("id_format_mismatch"))
        << QJsonDocument(wrong).toJson().toStdString();

    // Nothing was written by either refusal.
    QFile f(root + QStringLiteral("/ROADMAP.md"));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    EXPECT_EQ(f.readAll(), before) << "INV-8: a refusal writes nothing";
    f.close();

    // And the conforming id still lands.
    const QJsonObject good = appendStable(QStringLiteral("DECL-0042"));
    EXPECT_TRUE(good.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(good).toJson().toStdString();
}

// An UNDECLARED project keeps today's behaviour, including the stable-prefix
// ids two shipped suites assert on. This is the case that makes the departure
// from the design spec's § 2.3 table safe, and it is asserted rather than
// argued.
TEST(roadmap_id_format_declared, Inv8UndeclaredProjectStillAcceptsStableIds) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    QByteArray md = "# Roadmap\n\n";
    for (int i = 0; i < 30; ++i)
        md += "Padding to clear the minimum-parseable-size gate. \n";
    md += "\n## Work\n\n- \xE2\x9C\x85 [Ts20-SP1] **Existing bullet.**\n\n";
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"), md));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")]  = root;
    req[QStringLiteral("op")]          = QStringLiteral("append");
    req[QStringLiteral("section")]     = QStringLiteral("work");
    req[QStringLiteral("status")]      = QStringLiteral("planned");
    req[QStringLiteral("headline")]    = QStringLiteral("A new item.");
    req[QStringLiteral("kind")]        = QStringLiteral("implement");
    req[QStringLiteral("source")]      = QStringLiteral("test");
    req[QStringLiteral("id_strategy")] = QStringLiteral("stable_prefix");
    req[QStringLiteral("stable_id")]   = QStringLiteral("Ts20-SP6");
    const QJsonObject resp = rc.cmdRoadmapLogAppendForTest(req).object();
    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << "INV-8: `Ts20-SP6` fails the universal grammar and is the "
           "documented stable_id example — an undeclared project must still "
           "accept it\n"
        << QJsonDocument(resp).toJson().toStdString();
}

// ---------------------------------------------------------------- INV-9 ----

TEST(roadmap_id_format_declared, Inv9WriteTimeRefusalsLeaveTheFileAlone) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    const QString settings = root + QStringLiteral("/.ants/project.json");

    struct Case { const char *why; QJsonObject fmt; };
    QJsonObject badPrefix;  badPrefix[QStringLiteral("prefix")]  = QStringLiteral("2026");
    QJsonObject badType;    badType[QStringLiteral("pattern")]   = 7;
    QJsonObject badCompile; badCompile[QStringLiteral("pattern")] = QStringLiteral("([");
    QJsonObject tooLong;    tooLong[QStringLiteral("pattern")]
        = QStringLiteral("a").repeated(RoadmapParse::kIdFormatPatternMaxBytes + 1);
    const QVector<Case> cases = {
        {"a letter-free prefix", badPrefix},
        {"a non-string pattern", badType},
        {"a pattern that does not compile", badCompile},
        {"a pattern over the byte cap", tooLong},
    };

    for (const Case &c : cases) {
        RemoteControl rc(nullptr);
        QJsonObject req;
        req[QStringLiteral("caller_cwd")] = root;
        req[QStringLiteral("op")]         = QStringLiteral("set");
        req[QStringLiteral("id_format")]  = c.fmt;
        const QJsonObject resp = rc.cmdProjectSettings(req).object();
        EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool())
            << "INV-9: " << c.why << " must refuse\n"
            << QJsonDocument(resp).toJson().toStdString();
        EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
                  QStringLiteral("bad_args"))
            << "INV-9: " << c.why;
        EXPECT_FALSE(QFileInfo::exists(settings))
            << "INV-9: " << c.why << " — the file on disk must be unchanged";
    }

    // The valid form is accepted and lands.
    QJsonObject ok;
    ok[QStringLiteral("prefix")]  = QStringLiteral("DECL");
    ok[QStringLiteral("pattern")] = QString::fromUtf8(kPattern);
    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("op")]         = QStringLiteral("set");
    req[QStringLiteral("id_format")]  = ok;
    const QJsonObject resp = rc.cmdProjectSettings(req).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    const IdFormat fmt = ProjectSettings::idFormatFor(root);
    EXPECT_EQ(fmt.prefix,  QStringLiteral("DECL"));
    EXPECT_EQ(fmt.pattern, QString::fromUtf8(kPattern));
}

// --------------------------------------------------------------- INV-10 ----

TEST(roadmap_id_format_declared, Inv10LoadDropsAnInvalidMemberNotTheFile) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"), "# Roadmap\n"));

    // A valid prefix beside an uncompilable pattern, plus a path key that must
    // survive the drop.
    QJsonObject fmt;
    fmt[QStringLiteral("prefix")]  = QStringLiteral("DECL");
    fmt[QStringLiteral("pattern")] = QStringLiteral("(");
    QJsonObject o;
    o[QStringLiteral("id_format")] = fmt;
    o[QStringLiteral("roadmap")]   = QStringLiteral("ROADMAP.md");
    ASSERT_TRUE(writeFile(root + QStringLiteral("/.ants/project.json"),
                          QJsonDocument(o).toJson()));

    const ProjectSettings::Settings s = ProjectSettings::load(root);
    ASSERT_TRUE(s.roadmap.has_value())
        << "INV-10: an invalid id_format member must not take the rest of the "
           "settings down with it";
    EXPECT_EQ(*s.roadmap, QStringLiteral("ROADMAP.md"));
    ASSERT_TRUE(s.idFormat.has_value());
    EXPECT_EQ(s.idFormat->prefix, QStringLiteral("DECL"));
    EXPECT_TRUE(s.idFormat->pattern.isEmpty())
        << "INV-10: the uncompilable pattern is DROPPED, and the valid prefix "
           "beside it survives";
}

// --------------------------------------------------------------- INV-12 ----

TEST(roadmap_id_format_declared, Inv12DeclaredMatchMigratesAsParsed) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"), gfmDoc()));

    const auto census = [&](const IdFormat &fmt) {
        QString err;
        const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
        EXPECT_TRUE(disc.has_value()) << err.toStdString();
        if (!disc) return QHash<QString, int>{};
        const auto plan = RoadmapMigrate::planFrom(
            *disc, QStringLiteral("Fixture"), QStringLiteral("fixture"), fmt);
        QHash<QString, int> out;
        for (const auto &it : plan.items) ++out[it.idOrigin];
        return out;
    };

    IdFormat fmt; fmt.pattern = QString::fromUtf8(kPattern);
    const auto plain = census(IdFormat{});
    const auto decl  = census(fmt);

    EXPECT_GT(plain.value(QStringLiteral("quarantined")), 0)
        << "precondition: undeclared, the Vestige shape quarantines";
    EXPECT_GT(decl.value(QStringLiteral("parsed")),
              plain.value(QStringLiteral("parsed")))
        << "INV-12: a matched lead-in must migrate as `parsed`, not "
           "`quarantined` — otherwise the declaration is inert exactly where "
           "ANTS-4491 reads it";
    EXPECT_LT(decl.value(QStringLiteral("quarantined")),
              plain.value(QStringLiteral("quarantined")));
}

// The declared arm never NARROWS: a conforming bracket id still migrates as
// parsed under a pattern that does not match it.
TEST(roadmap_id_format_declared, Inv12DeclaredArmNeverNarrows) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"),
        "# Roadmap\n\n## Work\n\n"
        "- \xE2\x9C\x85 [ANTS-0042] **A conforming bullet.**\n"));

    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    ASSERT_TRUE(disc.has_value()) << err.toStdString();
    IdFormat fmt; fmt.pattern = QStringLiteral("^(ZZ\\d+)$");   // matches nothing here
    const auto plan = RoadmapMigrate::planFrom(
        *disc, QStringLiteral("Fixture"), QStringLiteral("fixture"), fmt);
    ASSERT_EQ(plan.items.size(), 1);
    EXPECT_EQ(plan.items.at(0).idOrigin, QStringLiteral("parsed"))
        << "INV-12: the universal arm is unconditional — declaring a pattern "
           "does not make ANTS-0042 off-grammar";
}

// --------------------------------------------------------------- INV-13 ----

// roadmap_query and roadmap_log's markdown read resolve ONE id for one bullet
// of a declaring project. The failure this forbids is a caller that omits the
// declaration and reads the same file as undeclared.
TEST(roadmap_id_format_declared, Inv13EveryPathResolvesOneId) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"), gfmDoc()));
    ASSERT_TRUE(declare(root, QString(), QString::fromUtf8(kPattern)));

    // Path 1 — roadmap_query.
    RemoteControl rc(nullptr);
    QJsonObject q;
    q[QStringLiteral("caller_cwd")] = root;
    q[QStringLiteral("id")]         = QStringLiteral("AX1");
    const QJsonObject qr = rc.cmdRoadmapQuery(q).object();
    ASSERT_TRUE(qr.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(qr).toJson().toStdString();
    EXPECT_EQ(qr.value(QStringLiteral("bullets")).toArray().size(), 1)
        << "INV-13: roadmap_query must address the bullet by its DECLARED id\n"
        << QJsonDocument(qr).toJson().toStdString();

    // Path 2 — roadmap_log's markdown read, via an annotate locator.
    QJsonObject a;
    a[QStringLiteral("caller_cwd")] = root;
    a[QStringLiteral("op")]         = QStringLiteral("annotate");
    a[QStringLiteral("id")]         = QStringLiteral("AX1");
    a[QStringLiteral("note")]       = QStringLiteral("Progress: reached.");
    a[QStringLiteral("dry_run")]    = true;
    const QJsonObject ar = rc.cmdRoadmapLogFlipForTest(a).object();
    EXPECT_TRUE(ar.value(QStringLiteral("ok")).toBool())
        << "INV-13: roadmap_log's markdown read must resolve the same id\n"
        << QJsonDocument(ar).toJson().toStdString();

    // Path 3 — the parser directly, which is the answer the other two must
    // agree with.
    const auto bs = RoadmapParse::parseBullets(
        QString::fromUtf8(gfmDoc()), ProjectSettings::idFormatFor(root));
    EXPECT_NE(byId(bs, "AX1"), nullptr);
}

// ---------------------------------------------------------------- INV-6 ----

// The store backend is untouched. Driven the realistic way: write markdown,
// migrate it, then read it back through the seam WITH a declaration in hand.
// A declaration that would rewrite every id on the markdown path must change
// nothing here, because every record is derived from bulletText(), which
// emits ants-v1.
TEST(roadmap_id_format_declared, Inv6StorePathIsUnaffected) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"),
        "# Roadmap\n\n## Work\n\n"
        "- \xE2\x9C\x85 [ANTS-0042] **A conforming bullet.**\n"
        "  Kind: implement\n"
        "  Source: test\n"));

    const QString storePath = root + QStringLiteral("/store.sqlite");
    auto store = openStore(storePath, RoadmapStore::Access::Bulk);
    ASSERT_NE(store, nullptr);

    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    ASSERT_TRUE(disc.has_value()) << err.toStdString();
    const auto plan = RoadmapMigrate::planFrom(
        *disc, QStringLiteral("Fixture"), QStringLiteral("fixture"));
    RoadmapMigrateLoad::Options opts;
    opts.changedAt   = QStringLiteral("2026-08-21T00:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(*store, plan, opts);
    ASSERT_TRUE(out.ok) << out.error.toStdString();

    auto reader = openStore(storePath, RoadmapStore::Access::Interactive);
    ASSERT_NE(reader, nullptr);
    auto text = RoadmapSource::RoadmapText::fromFile(
        root + QStringLiteral("/ROADMAP.md"));

    IdFormat wild;
    wild.pattern = QStringLiteral("^(.+)$");   // would rewrite every GFM id
    RoadmapSource::ReadError why = RoadmapSource::ReadError::None;
    QString rerr;
    const auto plainRecs = RoadmapSource::bulletsFor(
        *reader, root, text, /*includeArchive=*/false, &why, &rerr);
    auto text2 = RoadmapSource::RoadmapText::fromFile(
        root + QStringLiteral("/ROADMAP.md"));
    const auto declRecs = RoadmapSource::bulletsFor(
        *reader, root, text2, /*includeArchive=*/false, &why, &rerr, wild);

    ASSERT_TRUE(plainRecs.has_value()) << rerr.toStdString();
    ASSERT_TRUE(declRecs.has_value()) << rerr.toStdString();
    ASSERT_EQ(plainRecs->size(), declRecs->size());
    ASSERT_GT(plainRecs->size(), 0);
    for (int i = 0; i < plainRecs->size(); ++i) {
        EXPECT_EQ(plainRecs->at(i).id,       declRecs->at(i).id)       << i;
        EXPECT_EQ(plainRecs->at(i).boldId,   declRecs->at(i).boldId)   << i;
        EXPECT_EQ(plainRecs->at(i).headline, declRecs->at(i).headline) << i;
    }
}
