// ANTS-4667 — op:"amend_field": write one trailer COLUMN after creation.
// Contract: tests/features/roadmap_log_amend_field/spec.md
//
// Behavioural, through roadmap_log itself. Every case migrates a small
// markdown fixture into a store at RoadmapStore::defaultPath() (redirected
// into the case's sandbox) and re-opens the store to assert what landed.
//
// The fixture carries BOTH shapes deliberately, because the shadowing rule is
// the whole subtlety: DEMO-0007 ends on prose so its declarations stay in the
// stored body (shadowed), while DEMO-0003 ends on a TRAILING run of trailer
// lines, which ANTS-4506 strips out of the body — so its values live only in
// the columns, which is the state amend_field is for.

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
#include <optional>
#include <string>

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

bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
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
        // Ends on PROSE, so the declarations stay in the stored body.
        "- \xF0\x9F\x93\x8B [DEMO-0007] **An open item.**\n"
        "  Layman: A body-declared thing.\n"
        "  Kind: implement.\n"
        "  Source: seed.\n"
        "  Closing prose line.\n"
        "\n"
        // Ends on a TRAILING trailer run, which ANTS-4506 strips from the
        // body — so these values live only in the columns.
        "- \xE2\x9C\x85 [DEMO-0003] **A shipped item.**\n"
        "  Some prose about it.\n"
        "  Layman: A column-only thing.\n"
        "  Kind: fix.\n"
        "  Source: seed.\n"
        "  Lanes: vt, core.\n"
        "\n";
    return b;
}

QString seedMigrated(ants_test::XdgGuard &guard, const QTemporaryDir &tmp,
                     qint64 *projectId) {
    guard.setEnv("XDG_DATA_HOME",
                 QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
    const QString rawRoot = QDir(tmp.path()).filePath(QStringLiteral("proj"));
    if (!writeFile(rawRoot + QStringLiteral("/ROADMAP.md"), fixture()))
        return QString();
    const QString root = QFileInfo(rawRoot).canonicalFilePath();

    auto store = openStore(RoadmapStore::Access::Bulk);
    if (!store) return QString();
    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    if (!disc) { ADD_FAILURE() << "findRoadmaps: " << err.toStdString(); return QString(); }
    const auto plan =
        RoadmapMigrate::planFrom(*disc, QStringLiteral("Demo"), QStringLiteral("demo"));
    RoadmapMigrateLoad::Options opts;
    opts.changedAt   = QStringLiteral("2026-08-05T10:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(*store, plan, opts);
    if (!out.ok) { ADD_FAILURE() << "migration load: " << out.error.toStdString(); return QString(); }
    *projectId = out.projectId;
    return root;
}

QJsonObject fieldReq(const QString &root, const QString &id,
                     const QString &field, const QJsonValue &value) {
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("op")]         = QStringLiteral("amend_field");
    req[QStringLiteral("id")]         = id;
    req[QStringLiteral("field")]      = field;
    req[QStringLiteral("value")]      = value;
    return req;
}

std::optional<RoadmapStore::ItemWrite> itemOf(const QString &id, qint64 projectId) {
    auto store = openStore(RoadmapStore::Access::Interactive);
    if (!store) return std::nullopt;
    QString err;
    const auto pk = store->findItem(projectId, id, &err);
    if (!pk) { ADD_FAILURE() << "findItem: " << err.toStdString(); return std::nullopt; }
    return store->readItem(*pk, &err);
}

QString roadmapPath(const QString &root) {
    return QDir(root).filePath(QStringLiteral("ROADMAP.md"));
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
};

}  // namespace

// ---------------------------------------------------------------- INV-1 -----

TEST(RoadmapLogAmendField, Inv1SetsColumnAndRenders) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogAmendFieldForTest(
        fieldReq(fx.root, QStringLiteral("DEMO-0003"), QStringLiteral("layman"),
                 QStringLiteral("A corrected sentence for the card face."))).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    // No trailing period: the trailer parser strips a value's final full stop,
    // so the COLUMN holds "A column-only thing" though the line said
    // "A column-only thing." Asserting the stored form keeps this test about
    // amend_field rather than about the parser.
    EXPECT_EQ(resp.value(QStringLiteral("previous")).toString(),
              QStringLiteral("A column-only thing"))
        << "the envelope echoes what it replaced";

    const auto item = itemOf(QStringLiteral("DEMO-0003"), fx.projectId);
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item->layman, QStringLiteral("A corrected sentence for the card face."));
    EXPECT_TRUE(has(readAll(roadmapPath(fx.root)).toStdString(),
                    "A corrected sentence for the card face."))
        << "and the render published it";
}

// ---------------------------------------------------------------- INV-2 -----

TEST(RoadmapLogAmendField, Inv2ShadowedByBodyRefuses) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    // DEMO-0007's body declares `Layman:` at a line start.
    const QJsonObject resp = rc.cmdRoadmapLogAmendFieldForTest(
        fieldReq(fx.root, QStringLiteral("DEMO-0007"), QStringLiteral("layman"),
                 QStringLiteral("Would be invisible."))).object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("field_shadowed_by_body"))
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_TRUE(has(resp.value(QStringLiteral("error")).toString().toStdString(),
                    "amend_body"))
        << "the refusal names the route that works";

    const auto item = itemOf(QStringLiteral("DEMO-0007"), fx.projectId);
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item->layman, QStringLiteral("A body-declared thing"))
        << "nothing written under a shadowing declaration";
}

// ---------------------------------------------------------------- INV-3 -----

TEST(RoadmapLogAmendField, Inv3UnknownFieldRefused) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogAmendFieldForTest(
        fieldReq(fx.root, QStringLiteral("DEMO-0003"), QStringLiteral("headline"),
                 QStringLiteral("nope"))).object();
    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(), QStringLiteral("bad_args"));
    EXPECT_TRUE(has(resp.value(QStringLiteral("error")).toString().toStdString(),
                    "amend_headline"))
        << "headline has its own op and the refusal says so";
}

// ---------------------------------------------------------------- INV-4 -----

TEST(RoadmapLogAmendField, Inv4NotNullEmptyRefused) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QJsonObject bad = rc.cmdRoadmapLogAmendFieldForTest(
        fieldReq(fx.root, QStringLiteral("DEMO-0003"), QStringLiteral("source"),
                 QString())).object();
    EXPECT_FALSE(bad.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(bad.value(QStringLiteral("code")).toString(), QStringLiteral("bad_args"))
        << QJsonDocument(bad).toJson().toStdString();

    // layman IS nullable, so the same shape is accepted there.
    const QJsonObject ok = rc.cmdRoadmapLogAmendFieldForTest(
        fieldReq(fx.root, QStringLiteral("DEMO-0003"), QStringLiteral("layman"),
                 QString())).object();
    EXPECT_NE(ok.value(QStringLiteral("code")).toString(), QStringLiteral("bad_args"))
        << "layman is the one nullable trailer column";
}

// ---------------------------------------------------------------- INV-5 -----

TEST(RoadmapLogAmendField, Inv5DryRunWritesNothing) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    QJsonObject req = fieldReq(fx.root, QStringLiteral("DEMO-0003"),
                               QStringLiteral("layman"),
                               QStringLiteral("Previewed only."));
    req[QStringLiteral("dry_run")] = true;
    const QJsonObject resp = rc.cmdRoadmapLogAmendFieldForTest(req).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_TRUE(resp.value(QStringLiteral("dry_run")).toBool());

    const auto item = itemOf(QStringLiteral("DEMO-0003"), fx.projectId);
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item->layman, QStringLiteral("A column-only thing"))
        << "a preview must not write the column";
    EXPECT_FALSE(has(readAll(roadmapPath(fx.root)).toStdString(), "Previewed only."));
}

// ---------------------------------------------------------------- INV-6 -----

TEST(RoadmapLogAmendField, Inv6LanesAcceptsArray) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    QJsonArray lanes;
    lanes.append(QStringLiteral("mcp"));
    lanes.append(QStringLiteral("roadmap-store"));
    const QJsonObject resp = rc.cmdRoadmapLogAmendFieldForTest(
        fieldReq(fx.root, QStringLiteral("DEMO-0003"),
                 QStringLiteral("lanes"), lanes)).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    const auto item = itemOf(QStringLiteral("DEMO-0003"), fx.projectId);
    ASSERT_TRUE(item.has_value());
    ASSERT_EQ(item->lanes.size(), 2);
    EXPECT_EQ(item->lanes.at(0), QStringLiteral("mcp"));
    EXPECT_EQ(item->lanes.at(1), QStringLiteral("roadmap-store"));
}

// ---------------------------------------------------------------- INV-7 -----

// The trap's own redirect: the half that fires for a caller who has NOT read
// this contract, which is how the defect was met in the first place.
TEST(RoadmapLogAmendField, Inv7AmendBodyRedirectsToAmendField) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = fx.root;
    req[QStringLiteral("op")]         = QStringLiteral("amend_body");
    req[QStringLiteral("id")]         = QStringLiteral("DEMO-0003");
    req[QStringLiteral("old_text")]   = QStringLiteral("Layman: A column-only thing.");
    req[QStringLiteral("new_text")]   = QStringLiteral("Layman: Something else.");
    const QJsonObject resp = rc.cmdRoadmapLogAmendBodyForTest(req).object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("body_match_not_found"));
    EXPECT_TRUE(has(resp.value(QStringLiteral("hint")).toString().toStdString(),
                    "amend_field"))
        << "a composed trailer line is not in the body; the hint must say where "
           "it is: " << QJsonDocument(resp).toJson().toStdString();
}

// ANTS-4807 — a body_match_not_found shows the text it matched AGAINST, so a
// caller can see the difference instead of bisecting old_text to infer it.
// Pressless spent eleven probes on one miss and still reached a wrong
// diagnosis, which was unreachable by bisection: the stored body is not the
// body roadmap_query include_body:true returns.
//
// DEMO-0003 is exactly that gap. Its fixture ends on a trailing trailer run,
// which ANTS-4506 strips from the stored body into the columns — so the
// rendered body a caller reads carries Layman/Kind/Source/Lanes lines that the
// text this op searches has never held.
TEST(RoadmapLogAmendField, Ants4807NotFoundShowsWhatItMatchedAgainst) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = fx.root;
    req[QStringLiteral("op")]         = QStringLiteral("amend_body");
    req[QStringLiteral("id")]         = QStringLiteral("DEMO-0003");
    // No trailer label: the trailer hint returns its own envelope early, and
    // this case is the general miss that hint cannot anticipate.
    req[QStringLiteral("old_text")]   = QStringLiteral("a phrase that is not there");
    req[QStringLiteral("new_text")]   = QStringLiteral("x");
    const QJsonObject resp = rc.cmdRoadmapLogAmendBodyForTest(req).object();

    ASSERT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    ASSERT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("body_match_not_found"));

    const QString tail = resp.value(QStringLiteral("body_tail")).toString();
    EXPECT_TRUE(has(tail.toStdString(), "Some prose about it."))
        << "body_tail must be the stored body: "
        << QJsonDocument(resp).toJson().toStdString();
    // The assertion that carries the point: what the caller READ is not what
    // was searched, and the echo is what makes that visible in one call.
    EXPECT_FALSE(has(tail.toStdString(), "Layman:"))
        << "body_tail must be the STORED body, not the rendered one";
    EXPECT_GT(resp.value(QStringLiteral("body_chars")).toInt(), 0);
}

// ANTS-4813 — roadmap_query names which of `body`'s trailer lines the RENDER
// composed, so a caller can tell what amend_body can reach. The fixture is
// built for exactly this contrast: DEMO-0003 ends on a trailing trailer run,
// which is stripped into the columns, and DEMO-0007 ends on prose, so its
// declarations stay in the stored body.
TEST(RoadmapLogAmendField, Ants4813ComposedTrailersAreNamed) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);

    auto bulletFor = [&](const QString &id) {
        QJsonObject req;
        req[QStringLiteral("caller_cwd")]   = fx.root;
        req[QStringLiteral("id")]           = id;
        req[QStringLiteral("include_body")] = true;
        const QJsonObject resp = rc.cmdRoadmapQuery(req).object();
        EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
            << QJsonDocument(resp).toJson().toStdString();
        return resp.value(QStringLiteral("bullets")).toArray()
                   .at(0).toObject();
    };
    auto keys = [](const QJsonObject &o) {
        QStringList out;
        for (const QJsonValue &v :
                 o.value(QStringLiteral("composed_trailers")).toArray())
            out << v.toString();
        out.sort();
        return out;
    };

    // Column-only: the render wrote these lines, so amend_body cannot reach
    // them however plainly they appear in `body`.
    const QJsonObject columnOnly = bulletFor(QStringLiteral("DEMO-0003"));
    EXPECT_TRUE(columnOnly.value(QStringLiteral("body")).toString()
                    .contains(QStringLiteral("Layman:")))
        << "the premise: the composed line IS in the body a caller reads";
    EXPECT_TRUE(keys(columnOnly).contains(QStringLiteral("layman")));
    EXPECT_TRUE(keys(columnOnly).contains(QStringLiteral("kind")));

    // Body-declared: the author wrote these, so they are stored prose and
    // amend_body owns them. Naming them here would send a caller to the wrong
    // op just as surely as naming none does.
    const QJsonObject bodyDeclared = bulletFor(QStringLiteral("DEMO-0007"));
    EXPECT_FALSE(keys(bodyDeclared).contains(QStringLiteral("layman")))
        << "a body-declared trailer is stored prose, not a composed line";
    EXPECT_FALSE(keys(bodyDeclared).contains(QStringLiteral("kind")));
}

// ANTS-4808 — op:"set_body" replaces a body outright, which is the route back
// for text amend_body cannot express as a match. Three sessions reported
// ending at "there is no route at all"; one left a kilobyte of garbled prose
// in a shipped roadmap item because of it.
TEST(RoadmapLogSetBody, Ants4808ReplacesTheWholeBody) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);

    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = fx.root;
    req[QStringLiteral("id")]         = QStringLiteral("DEMO-0007");
    // The new body declares Layman: itself. DEMO-0007 is OPEN and its Layman
    // lives in its body, so a replacement that dropped the declaration would
    // clear the column and the render gate would refuse the write — correctly.
    // That interaction is asserted on its own below.
    req[QStringLiteral("new_text")] =
        QStringLiteral("Layman: A rewritten thing.\nRewritten from scratch.");
    const QJsonObject resp = rc.cmdRoadmapLogSetBodyForTest(req).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("op")).toString(),
              QStringLiteral("set_body"));
    // The reply says what it destroyed — the one op whose cost a caller
    // cannot read off its own arguments.
    EXPECT_GT(resp.value(QStringLiteral("replaced_body_chars")).toInt(), 0);

    QJsonObject q;
    q[QStringLiteral("caller_cwd")]   = fx.root;
    q[QStringLiteral("id")]           = QStringLiteral("DEMO-0007");
    q[QStringLiteral("include_body")] = true;
    const QJsonObject after = rc.cmdRoadmapQuery(q).object();
    const QString body = after.value(QStringLiteral("bullets")).toArray()
                             .at(0).toObject()
                             .value(QStringLiteral("body")).toString();
    EXPECT_TRUE(body.contains(QStringLiteral("Rewritten from scratch.")));
    EXPECT_FALSE(body.contains(QStringLiteral("Closing prose line.")))
        << "the previous prose must be gone, or this is an append";
}

// ANTS-4808 — old_text is REFUSED rather than ignored. A caller who sent one
// believes the write is bounded by it, so discarding it silently would
// destroy text they thought they had protected.
TEST(RoadmapLogSetBody, Ants4808OldTextIsRefusedNotIgnored) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);

    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = fx.root;
    req[QStringLiteral("id")]         = QStringLiteral("DEMO-0007");
    req[QStringLiteral("old_text")]   = QStringLiteral("Closing prose line.");
    req[QStringLiteral("new_text")]   = QStringLiteral("x");
    const QJsonObject resp = rc.cmdRoadmapLogSetBodyForTest(req).object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_op_combo"));
    EXPECT_TRUE(has(resp.value(QStringLiteral("error")).toString().toStdString(),
                    "amend_body"))
        << "the refusal must name the op that DOES take old_text";
}

// ANTS-4808 — replacing an OPEN item's body drops any Layman: it declared
// there, which clears the column, which the render gate then refuses. The gate
// is right and the op must not sidestep it: a rescue that silently strips a
// required field would trade one kind of damage for another. Asserted so the
// interaction is a documented cost of the op rather than a surprise.
TEST(RoadmapLogSetBody, Ants4808StrippingAnOpenItemsLaymanIsRefused) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);

    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = fx.root;
    req[QStringLiteral("id")]         = QStringLiteral("DEMO-0007");
    req[QStringLiteral("new_text")]   = QStringLiteral("No Layman line here.");
    const QJsonObject resp = rc.cmdRoadmapLogSetBodyForTest(req).object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool())
        << "an open item left with no Layman must not be written";
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("render_gate_unmet"));
    // The refusal names the remedy and says it can ride along in the same
    // call, which is what keeps the op usable in the case it exists for.
    EXPECT_TRUE(has(resp.value(QStringLiteral("error")).toString().toStdString(),
                    "Layman"));
}

// ANTS-4808 — a dry run previews and writes nothing, like every other op here.
TEST(RoadmapLogSetBody, Ants4808DryRunWritesNothing) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);

    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = fx.root;
    req[QStringLiteral("id")]         = QStringLiteral("DEMO-0007");
    req[QStringLiteral("new_text")] =
        QStringLiteral("Layman: A previewed thing.\nPreview only.");
    req[QStringLiteral("dry_run")]    = true;
    const QJsonObject resp = rc.cmdRoadmapLogSetBodyForTest(req).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_TRUE(resp.value(QStringLiteral("dry_run")).toBool());

    QJsonObject q;
    q[QStringLiteral("caller_cwd")]   = fx.root;
    q[QStringLiteral("id")]           = QStringLiteral("DEMO-0007");
    q[QStringLiteral("include_body")] = true;
    const QString body = rc.cmdRoadmapQuery(q).object()
                             .value(QStringLiteral("bullets")).toArray()
                             .at(0).toObject()
                             .value(QStringLiteral("body")).toString();
    EXPECT_FALSE(body.contains(QStringLiteral("Preview only.")))
        << "a preview must not land";
    EXPECT_TRUE(body.contains(QStringLiteral("Closing prose line.")))
        << "and must not destroy what it previewed replacing";
}

// ---------------------------------------------------------------- INV-8 -----

TEST(RoadmapLogAmendField, Inv8UnknownIdRefused) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogAmendFieldForTest(
        fieldReq(fx.root, QStringLiteral("DEMO-9999"), QStringLiteral("layman"),
                 QStringLiteral("x"))).object();
    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("bullet_not_found"))
        << QJsonDocument(resp).toJson().toStdString();
}

// ---------------------------------------------------------------------------
// ANTS-4750 — an unrecognised Kind refuses `bad_kind`, not `bad_args`.
//
// Found by a cold lane on mcp-error-codes.md, which opened both call sites and
// did not presume which was canonical. The taxonomy now says: a named code
// owns its enum where one exists (the `bad_args` row was corrected during the
// ANTS-4435 gate). roadmap_log's own `kind` argument already refuses
// `bad_kind`; this path refused `bad_args` for the same bad value.
//
// The harm is a caller branching on `code` to re-prompt for a valid Kind: it
// handles `bad_kind` and silently mis-handles this path — which is the split
// `bad_kind` was minted to end.
//
// The item said to check the sibling fields in the same handler first. There
// is no sibling split: `field` accepts layman|kind|source|lanes|evidence, and
// `status` is not among them — it is op:"flip"'s. `kind` is the only enum this
// op writes.
TEST(RoadmapLogAmendField, Ants4750UnrecognisedKindIsBadKind) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogAmendFieldForTest(
        fieldReq(fx.root, QStringLiteral("DEMO-0003"), QStringLiteral("kind"),
                 QStringLiteral("weird"))).object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_kind"))
        << "the same bad value through roadmap_log's own `kind` argument "
           "refuses bad_kind; one code per enum is the whole point";

    // And name the vocabulary, as every other enum refusal in this MCP does —
    // otherwise the caller that just learned the code still has to guess.
    const QJsonArray accepted =
        resp.value(QStringLiteral("accepted")).toArray();
    ASSERT_FALSE(accepted.isEmpty());
    bool sawFix = false;
    for (const auto &v : accepted)
        if (v.toString() == QStringLiteral("fix")) sawFix = true;
    EXPECT_TRUE(sawFix) << "the accepted list must be the real Kind set";
}

// ANTS-4750 — the neighbouring NOT NULL refusal is a different failure and
// keeps its own code. Guarding it here because the fix edits the branch
// directly above it.
TEST(RoadmapLogAmendField, Ants4750EmptyValueStillBadArgs) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogAmendFieldForTest(
        fieldReq(fx.root, QStringLiteral("DEMO-0003"), QStringLiteral("kind"),
                 QString())).object();
    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_args"))
        << "an empty value is a NOT NULL violation, not an unknown enum value";
}
